// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_SURFACE_H
#define TERRAIN_SURFACE_H

// The one implementation of the terrain's surface: height, normal, is-hole and ray (T1 R14-R16,
// programme D5). It matches what the terrain vertex shader draws: texel centres at (i + 0.5) / N,
// bilinear, clamped to the edge, within 1e-4 (S18:530, S26:467). Normals are a forward difference at
// one texel, which foliage and physics were placed with.
//
// Everything here is pure and thread-safe over its inputs: particle collisions call it from workers.

#include "Export.h"
#include "math/Vector3.h"
#include "terrain/TerrainHoles.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>

namespace jasperengine::terraincore {

    struct SurfaceView {
        const uint8_t* pixels = nullptr;
        int width = 0, height = 0, channels = 0, bytesPerChannel = 1;
        float size = 0.0f;
        float maxHeight = 0.0f;
        HoleMapView holes;
    };

    struct SurfaceSample {
        float height = 0.0f;
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        bool hole = false;      // defined height and normal stay under it (T3 R26)
        bool inside = false;    // the point is on the terrain; outside clamps to the edge
        bool present = false;   // a map answered; without one, height 0 and up
        bool exact = true;      // false: answered from an overview, not the map (T4 R24)
    };

    // Normalized height of the red channel of one texel, 8 or 16-bit (little-endian) or a 32-bit float
    inline float heightTexelAt(const unsigned char* pixels, size_t texelIndex, int channels, int bytesPerChannel) {
        const size_t index = texelIndex * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
        // a float map's CPU view (an .exr or .hdr source: TextureData's RGBA32F), as the shader reads it
        if (bytesPerChannel >= 4) {
            float value;
            std::memcpy(&value, pixels + index, sizeof(value));
            return std::isfinite(value) ? value : 0.0f;
        }
        if (bytesPerChannel >= 2) {
            const unsigned int value = static_cast<unsigned int>(pixels[index]) | (static_cast<unsigned int>(pixels[index + 1]) << 8);
            return static_cast<float>(value) / 65535.0f;
        }
        return pixels[index] / 255.0f;
    }

    // Texel coordinate of a terrain-local position along one axis of a size-texel map
    inline float surfaceTexelCoord(float local, float terrainSize, int size) {
        return ((local + terrainSize * 0.5f) / terrainSize) * static_cast<float>(size) - 0.5f;
    }

    // Normalized [0,1] height at a texel coordinate; a clamped coordinate blends the edge texel with
    // itself, as the clamp-to-edge sampler does
    JASPERENGINE_API float sampleHeightTexels(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY);
    // Normalized [0,1] height at a terrain-local position; 0 without pixels
    JASPERENGINE_API float sampleHeightLocal(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float localX, float localZ);

    // Never NaN: a point outside clamps to the edge with inside false
    JASPERENGINE_API SurfaceSample sampleSurface(const SurfaceView& view, float localX, float localZ);
    // Terrain-local height, the same as sampleSurface's with no normal to work out
    JASPERENGINE_API float sampleHeight(const SurfaceView& view, float localX, float localZ);
    // A march along origin + direction * t, t in [0, maxT], refined at the first crossing. point is
    // on the surface. skipHoles passes through a crossing inside a hole and goes on along the ray;
    // brush rays leave it false, so a hole can be filled (T3 R25).
    JASPERENGINE_API bool raycastSurface(const SurfaceView& view, const Vector3& origin, const Vector3& direction, float maxT,
                                         bool skipHoles, Vector3& point, float& t);

    // The abstract form for regions and worlds (T1 R93; T5 FoliageRegion, T4 R24)
    class TerrainSurfaceQuery {
    public:
        virtual ~TerrainSurfaceQuery() = default;
        virtual SurfaceSample sample(float localX, float localZ) const = 0;
        virtual float heightAt(float localX, float localZ) const = 0;
        virtual bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const = 0;
    };

    // The query over one view (T1 R93's single-map implementation)
    class JASPERENGINE_API SurfaceViewQuery final : public TerrainSurfaceQuery {
    public:
        explicit SurfaceViewQuery(const SurfaceView& view): view(view) {}
        SurfaceSample sample(float localX, float localZ) const override;
        float heightAt(float localX, float localZ) const override;
        bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const override;

    private:
        SurfaceView view;
    };

}

#endif // TERRAIN_SURFACE_H
