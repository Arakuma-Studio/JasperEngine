// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_HEIGHTFIELDEROSION_H
#define TERRAIN_HEIGHTFIELDEROSION_H

// Hydraulic and thermal erosion as heightfield stages of a Noise Graph (T2 R24-R27, N6), written from
// Beyer (2015) and Musgrave, Kolb and Mace (1989). Same seed and parameters give the same bits on any
// worker count and any target. Compiled with JASPERENGINE_NOISE (N8), so bakes pay nothing; the Erode
// brush is TerrainErosion and stays as it is.

#include "Export.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine { namespace jobs { class CancelToken; } }

namespace jasperengine { namespace terraincore {

    struct HydraulicParams {
        float dropletsPerTexel = 0.25f;
        int lifetime = 48;
        float inertia = 0.05f;
        float capacity = 4.0f;
        float minSlope = 0.01f;
        float erodeRate = 0.3f;
        float depositRate = 0.3f;
        float evaporation = 0.02f;
        float gravity = 4.0f;
        // Texels a cut or deposit spreads over. 2 wears the ground down without roughening it; 1 is
        // faster and preserves fine detail, and can roughen it (Deviations of docs/specs/T2-noise-graphs.md)
        int radius = 2;
        uint32_t seed = 0;
    };

    struct ThermalParams {
        int iterations = 8;
        float talusAngle = 35.0f;   // degrees
        float rate = 0.5f;
    };

    // Which global texels a buffer holds, and the world scale they stand for (R26)
    struct HeightfieldRegion {
        int64_t startX = 0, startZ = 0;
        int width = 0, depth = 0;
        float maxHeight = 100.0f;
        double metresPerTexel = 1.0;
    };

    struct ErosionControl {
        jobs::CancelToken* cancel = nullptr;
        bool parallel = true;
    };

    class JASPERENGINE_API HeightfieldErosion {
    public:
        // height: normalized, in and out. hardness: optional, 0..1, scales the cut down. flow and
        // sediment: optional, 0..1 out. False when cancelled, with height partly eroded: the caller
        // owns the only copy and drops it.
        static bool hydraulic(const HydraulicParams& params, const HeightfieldRegion& region, float* height, const float* hardness,
                              float* flow, float* sediment, const ErosionControl& control = {}, uint64_t* dropletSteps = nullptr);
        static bool thermal(const ThermalParams& params, const HeightfieldRegion& region, float* height, const ErosionControl& control = {});

        // The texels of padding a region needs around what it keeps (R22)
        static int hydraulicPadding(const HydraulicParams& params) { return params.lifetime + params.radius + 2; }
        static int thermalPadding(const ThermalParams& params) { return params.iterations; }
    };

}}

#endif // TERRAIN_HEIGHTFIELDEROSION_H
