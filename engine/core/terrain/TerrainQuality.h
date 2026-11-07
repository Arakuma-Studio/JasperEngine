// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_QUALITY_H
#define TERRAIN_QUALITY_H

// T3 R1, R2: the terrain material's Quality tiers. The table here is the single source: the shader's
// defines, the readouts and the collector all read it. Pure: no ECS and no renderer (ledger D1).

#include "Export.h"

#include <cstdint>

namespace jasperengine::terrain {

    // Stored as ints: append only
    enum class TerrainQuality : int { HIGH, MEDIUM, LOW };

    inline constexpr int TERRAIN_QUALITY_COUNT = 3;
    inline constexpr uint32_t TERRAIN_ALL_TIERS = 0x7u;

    struct TerrainTierInfo {
        int layersPerPixel;         // strongest layers kept, 0 = all
        int limitedLayersPerPixel;  // the same with Limit Layers Per Pixel on
        int readsPerMap;            // the most reads of one map at one pixel
        bool hexAndStochastic;      // otherwise those layers draw plain
        bool trimBlendMaps;         // read only the blend maps that hold layers (R7)
        bool surfaceMaps;           // the packed surface map is read
        bool normalForAllLayers;    // otherwise the strongest layer's only
        bool macroInVertexStage;
        bool macroNormal;
        bool distanceResampling;
        bool offscreenShadowsAuto;
        int detailArraySize;        // the most of a side of the detail array
        int worstFetches;           // R8, 0 = as many as the layers ask for
    };

    JASPERENGINE_API const TerrainTierInfo& terrainTierInfo(TerrainQuality quality);

    // lower-case names, as saved and as MCP and Lua take them: high, medium, low
    JASPERENGINE_API const char* terrainQualityName(TerrainQuality quality);
    JASPERENGINE_API bool terrainQualityFromName(const char* name, TerrainQuality& quality);

    // What decided a terrain's tier (R43's quality_source)
    enum class TerrainQualitySource : int { EDITOR_PREVIEW, GLOBAL_OVERRIDE, COMPONENT, PROJECT };

    struct TerrainQualityRequest {
        bool hasPreview = false;
        TerrainQuality preview = TerrainQuality::HIGH;
        bool hasOverride = false;
        TerrainQuality globalOverride = TerrainQuality::HIGH;
        bool hasComponent = false;
        TerrainQuality component = TerrainQuality::HIGH;
        TerrainQuality project = TerrainQuality::HIGH;
        uint32_t shippedMask = TERRAIN_ALL_TIERS;
    };

    struct TerrainQualityResolved {
        TerrainQuality quality = TerrainQuality::HIGH;
        TerrainQualitySource source = TerrainQualitySource::PROJECT;
        bool clamped = false;   // the tier asked for was not shipped: this is the nearest one
    };

    // R2: editor preview, else the runtime's global override, else the component's quality when not
    // Auto, else the project's tier of the running platform. A tier the build did not ship clamps to
    // the nearest shipped one, higher first.
    JASPERENGINE_API TerrainQualityResolved resolveTerrainQuality(const TerrainQualityRequest& request);
    JASPERENGINE_API TerrainQuality clampTerrainQuality(TerrainQuality quality, uint32_t shippedMask);

    inline constexpr uint32_t terrainQualityBit(TerrainQuality quality) {
        return uint32_t(1) << static_cast<int>(quality);
    }

    // What a Terrain says of its variant, for RenderSystem's and the collector's one rule (D2)
    struct TerrainVariantInputs {
        bool hasComponentQuality = false;
        TerrainQuality componentQuality = TerrainQuality::HIGH;
        bool limitLayers = false;
        int layerCount = 0;
        bool macro = false;
        bool macroNormal = false;
        bool resample = false;
        bool cliff = false;
        bool heightBlend = false;
        bool hex = false;
        bool hexStochastic = false;
    };

    // The Scene view's view of the terrain (R42): TERRAIN_DEBUG_VIEW's modes
    enum class TerrainDebugView : int { SHADED, LAYER_COUNT, FETCH_COST, HOLES };

    // The run time's global state of R2 and R34, as ShadowTiers keeps the project's shadow tiers: set
    // by the editor and by an exported game's startup code. Main thread.
    class JASPERENGINE_API TerrainQualitySettings {
    public:
        static constexpr int PLATFORM_COUNT = 6;    // Engine::Platform as an int

        // graphics.terrainQuality.<platform>; absent means High on every platform (R51)
        static void setPlatformTier(int platform, TerrainQuality quality);
        static TerrainQuality getPlatformTier(int platform);
        static TerrainQuality getCurrentPlatformTier();

        // PlayerConfig's shipped tiers (R34)
        static void setShippedMask(uint32_t mask);
        static uint32_t getShippedMask();

        // Engine.setTerrainQuality (R48)
        static void setGlobalOverride(TerrainQuality quality);
        static void clearGlobalOverride();
        static bool getGlobalOverride(TerrainQuality& quality);

        // The Scene view's preview (R42): not saved, and ended at Play
        static void setEditorPreview(TerrainQuality quality);
        static void clearEditorPreview();
        static bool getEditorPreview(TerrainQuality& quality);
        static void setDebugView(TerrainDebugView view);
        static TerrainDebugView getDebugView();

        // Bumped by each change above: whoever chose a variant by them looks again
        static uint64_t getRevision();

        // R34's one warning, for a request the build did not ship; true the first time
        static bool takeClampWarning();
        // Forgets the state above, for a project that closes and a test that starts
        static void reset();
    };

}

#endif // TERRAIN_QUALITY_H
