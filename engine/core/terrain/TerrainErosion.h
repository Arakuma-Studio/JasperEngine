// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_EROSION_H
#define TERRAIN_EROSION_H

// Erosion over a normalized height buffer, moved from editor/util with its results unchanged (T1
// R22). The kernels see a window of the map, so a stamp needs no whole-map buffer; the caller's
// weight is what keeps the effect inside the stamp.

#include "Export.h"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace jasperengine::terraincore {

    // Inclusive texel bounds
    struct ErosionRegion {
        int minX = 0, minY = 0, maxX = 0, maxY = 0;

        bool empty() const { return maxX < minX || maxY < minY; }
        int width() const { return maxX - minX + 1; }
        int height() const { return maxY - minY + 1; }
    };

    // heights holds windowWidth * windowHeight values for the texels starting at (windowX, windowY)
    // of a mapWidth * mapHeight map (coordinates relative to the map's own origin). It must reach two
    // texels past the region, clipped to the map: a droplet reads its cell's far corner.
    struct ErosionField {
        float* heights = nullptr;
        int windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0;
        int mapWidth = 0, mapHeight = 0;
    };

    // Weight for a texel, 0 outside the brush
    using ErosionWeightFn = float (*)(void* user, int x, int y);

    class JASPERENGINE_API TerrainErosion {

    public:

        // Droplets carry sediment downhill and drop it where the slope eases. heightScale converts
        // a height difference into texel steps, so any map resolution erodes alike.
        static void hydraulic(const ErosionField& field, const ErosionRegion& region, float heightScale, int droplets,
                              uint32_t seed, ErosionWeightFn weight, void* user);

        // Slopes steeper than talus shed onto their lower neighbours: the angle of repose
        static void thermal(const ErosionField& field, const ErosionRegion& region, float heightScale, float talus, float rate,
                            ErosionWeightFn weight, void* user);

        template<typename F>
        static void hydraulic(const ErosionField& field, const ErosionRegion& region, float heightScale, int droplets, uint32_t seed, F&& weight) {
            hydraulic(field, region, heightScale, droplets, seed, &call<std::remove_reference_t<F>>, const_cast<void*>(static_cast<const void*>(&weight)));
        }

        template<typename F>
        static void thermal(const ErosionField& field, const ErosionRegion& region, float heightScale, float talus, float rate, F&& weight) {
            thermal(field, region, heightScale, talus, rate, &call<std::remove_reference_t<F>>, const_cast<void*>(static_cast<const void*>(&weight)));
        }

    private:

        template<typename F>
        static float call(void* user, int x, int y) {
            return (*static_cast<F*>(user))(x, y);
        }
    };

}

#endif // TERRAIN_EROSION_H
