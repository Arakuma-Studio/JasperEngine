// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWTIERS_H
#define SHADOWTIERS_H

#include "Export.h"

#include <cstdint>

namespace jasperengine {

    // Shadow quality tier: what one platform can afford (docs/rendering/shadows.md). The scene's
    // Quality picks one (SceneSettings::shadowQuality), or AUTO takes the platform's from the
    // project (graphics.shadowTier.<platform>). Stored as ints: append only.
    enum class ShadowTier : int {
        LOW,     // default of Android and iOS
        MEDIUM,  // default of the web
        HIGH,    // default of Windows, Linux and macOS
        ULTRA    // opt-in: PCSS contact hardening
    };

    // the filter a Soft light gets on a tier
    enum class ShadowSoftFilter : int {
        GRID3,   // 3x3 bilinear grid (4 comparison fetches)
        GRID5,   // 5x5 bilinear grid (9 fetches)
        VOGEL,   // 16-tap Vogel disk rotated by world-locked noise
        PCSS     // blocker search + Vogel PCF of the physical penumbra
    };

    // how the receiver crosses from one directional cascade to the next
    enum class CascadeTransition : int {
        NONE,    // a hard switch at the split
        DITHER,  // one cascade per pixel, picked by world-locked noise against the blend weight
        BLEND    // both cascades sampled and mixed over the band
    };

    struct ShadowTierSettings {
        unsigned int maxCascades;           // directional cascades at most
        unsigned int slotResolution;        // the largest shadow map of a slot (texels per side)
        unsigned int spotResolution;        // a spot light's map at mapResolution 0 (Use Quality Settings)
        unsigned int pointResolution;       // a point light's cube face at mapResolution 0
        float shadowDistance;               // metres of directional shadows at shadowDistance 0
        ShadowSoftFilter softFilter;
        CascadeTransition transition;
        bool receiverPlane;                 // receiver-plane depth bias
        bool freezeFit;                     // cascades keep their fit while the camera stays inside it (caching)
        uint64_t memoryBytes;               // the atlases and static caches together
    };

    class JASPERENGINE_API ShadowTiers {
    public:
        static const ShadowTierSettings& getSettings(ShadowTier tier);

        // the reach of a tier's widest point-light kernel in texels around its centre (bilinear
        // footprint included): the guard band a cube face is rendered with
        static float pointKernelReach(ShadowTier tier);

        // the default tier of a platform (Engine::Platform as an int: MacOS, iOS, Web, Android,
        // Linux, Windows)
        static ShadowTier defaultTier(int platform);
        // the project's tier of a platform (graphics.shadowTier.<platform>): set by the editor and
        // by an exported game's startup code, the default until then
        static void setPlatformTier(int platform, ShadowTier tier);
        static ShadowTier getPlatformTier(int platform);
        static constexpr int PLATFORM_COUNT = 6;

        // lower-case names, as saved and as MCP and Lua take them: low, medium, high, ultra
        static const char* getName(ShadowTier tier);
        static bool fromName(const char* name, ShadowTier& tier);
    };

}

#endif // SHADOWTIERS_H
