// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainSurface.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // the bilinear height of the four texels around (texelX, texelY), clamped to the edge; one body
    // for every caller in this file, so the query's own path has no call between it and the pixels
    // (T1 R50: within 1.05x of the code it replaced)
    // pixels, width, height and channels already known valid. The clamp is also the NaN guard: a NaN
    // coordinate fails the > test and reads texel 0, where std::clamp would pass it to the int cast,
    // which is undefined; a separate test of the position cost the query about 8 % (R50)
    inline float bilinearHeightValid(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY) {
        texelX = texelX > 0.0f ? std::min(texelX, static_cast<float>(width - 1)) : 0.0f;
        texelY = texelY > 0.0f ? std::min(texelY, static_cast<float>(height - 1)) : 0.0f;
        const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, width - 1);
        const int lowerY = std::clamp(static_cast<int>(std::floor(texelY)), 0, height - 1);
        const int upperX = std::min(lowerX + 1, width - 1);
        const int upperY = std::min(lowerY + 1, height - 1);
        const float blendX = texelX - static_cast<float>(lowerX);
        const float blendY = texelY - static_cast<float>(lowerY);

        const size_t lowerRow = static_cast<size_t>(lowerY) * static_cast<size_t>(width);
        const size_t upperRow = static_cast<size_t>(upperY) * static_cast<size_t>(width);
        float h00, h10, h01, h11;
        if (channels == 1 && bytesPerChannel == 2) {
            // the common map, a 16-bit height map, read without heightTexelAt's format tests a texel
            auto texel = [&](size_t index) {
                return static_cast<float>(static_cast<unsigned int>(pixels[index * 2]) | (static_cast<unsigned int>(pixels[index * 2 + 1]) << 8)) / 65535.0f;
            };
            h00 = texel(lowerRow + lowerX);
            h10 = texel(lowerRow + upperX);
            h01 = texel(upperRow + lowerX);
            h11 = texel(upperRow + upperX);
        } else {
            h00 = heightTexelAt(pixels, lowerRow + lowerX, channels, bytesPerChannel);
            h10 = heightTexelAt(pixels, lowerRow + upperX, channels, bytesPerChannel);
            h01 = heightTexelAt(pixels, upperRow + lowerX, channels, bytesPerChannel);
            h11 = heightTexelAt(pixels, upperRow + upperX, channels, bytesPerChannel);
        }
        const float lower = h00 + (h10 - h00) * blendX;
        const float upper = h01 + (h11 - h01) * blendX;
        return lower + (upper - lower) * blendY;
    }

    inline float bilinearHeight(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY) {
        if (!pixels || width <= 0 || height <= 0 || channels <= 0) {
            return 0.0f;
        }
        return bilinearHeightValid(pixels, width, height, channels, bytesPerChannel, texelX, texelY);
    }

    inline float bilinearHeightLocal(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float localX, float localZ) {
        if (terrainSize <= std::numeric_limits<float>::epsilon()) {
            return 0.0f;
        }
        return bilinearHeight(pixels, width, height, channels, bytesPerChannel,
                              surfaceTexelCoord(localX, terrainSize, width), surfaceTexelCoord(localZ, terrainSize, height));
    }

}

float jasperengine::terrain::sampleHeightTexels(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY) {
    return bilinearHeight(pixels, width, height, channels, bytesPerChannel, texelX, texelY);
}

float jasperengine::terrain::sampleHeightLocal(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float localX, float localZ) {
    return bilinearHeightLocal(pixels, width, height, channels, bytesPerChannel, terrainSize, localX, localZ);
}

namespace {

    bool hasMap(const SurfaceView& view) {
        return view.pixels && view.width > 0 && view.height > 0 && view.channels > 0 &&
               view.size > std::numeric_limits<float>::epsilon();
    }

    // A NaN position would make the inside and hole tests' answers meaningless
    float finite(float value) {
        return value == value ? value : 0.0f;
    }

    bool holeAt(const SurfaceView& view, float localX, float localZ) {
        if (!view.holes.texels || view.size <= std::numeric_limits<float>::epsilon()) return false;
        const float half = view.size * 0.5f;
        return isHole(view.holes, (localX + half) / view.size, (localZ + half) / view.size);
    }

    float heightAtLocal(const SurfaceView& view, float x, float z) {
        // hasMap has checked the view: the query's own path tests nothing twice
        return view.maxHeight * bilinearHeightValid(view.pixels, view.width, view.height, view.channels, view.bytesPerChannel,
                                                    surfaceTexelCoord(x, view.size, view.width), surfaceTexelCoord(z, view.size, view.height));
    }

}

SurfaceSample jasperengine::terrain::sampleSurface(const SurfaceView& view, float localX, float localZ) {
    SurfaceSample result;

    if (view.size > std::numeric_limits<float>::epsilon()) {
        const float x = finite(localX);
        const float z = finite(localZ);
        const float half = view.size * 0.5f;
        result.inside = x >= -half && x <= half && z >= -half && z <= half;
        result.hole = holeAt(view, x, z);
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
    return heightAtLocal(view, localX, localZ);
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
