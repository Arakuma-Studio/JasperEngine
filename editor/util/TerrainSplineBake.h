// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The route resampling and segment-nearest evaluation follow OpenEngine's
// Engine/Modules/TerrainECS/{Include/TerrainECS,Source}/TerrainRoutePolyline.* and its
// SplinePath/SplineArea volumes (Engine/Include/Components/Terrain/TerrainModifierVolume.h),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#pragma once

#include "Project.h"
#include "component/SplineComponent.h"
#include "spline/SplineStation.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/TreeStore.h"
#include "util/TerrainMapUtils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // A map's pixels as the pure bake takes them (tests): it writes them through a store (T1 R20)
    struct TerrainBakeMap{
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        int channels = 0;
        int bytesPerChannel = 1;

        bool valid() const { return pixels && width > 0 && height > 0 && channels > 0; }
    };

    // Where the terrain is: its local frame (texel centres at (i + 0.5) / size of terrainSize,
    // centred on the origin; heights normalized to maxHeight) and its world matrix
    struct TerrainBakeFrame{
        Matrix4 localToWorld;
        Matrix4 worldToLocal;
        float terrainSize = 200.0f;
        float maxHeight = 5.0f;
    };

    // What one bake changed (S18 R11)
    struct TerrainBakeChanges{
        TerrainMapRegion heights;
        TerrainMapRegion blend;
        int blendMap = -1;
        // foliage layer -> its density map's changed rect
        std::vector<std::pair<int, TerrainMapRegion>> density;
        uint32_t texels = 0;
        std::vector<std::string> warnings;

        bool empty() const;
    };

    // The spline -> terrain bake (S18 R11, R12, D8): flattens the road bed (cut and fill), paints
    // a surface layer and clears foliage along a spline path, or over the inside of a closed
    // spline. It is destructive: a bake rewrites texels of the terrain's maps through applyField,
    // one undoable step ("Apply Spline to Terrain"), a TerrainMapDeltaCmd of the tiles it changed in
    // each map, so undo restores the maps bit-exactly (T1 R20).
    class TerrainSplineBake{
    public:
        // stations closer than this along the route are not resampled further (OpenEngine
        // kMinRouteStationSpacing)
        static constexpr float MIN_ROUTE_SPACING = 0.05f;

        // ---- the pure bake (tests drive it without a Project)

        // The route: stations resampled to at most half a texel apart (D8)
        // T5 R29: the trees within reach of a terrain-local XZ polyline
        static std::vector<uint32_t> treesInFootprint(const terrain::TreeStore& store, float terrainSize, const std::vector<Vector2>& line, float reach);
        static void routeStations(const std::vector<spline::SplineStation>& stations, float texelSize,
                                  std::vector<spline::SplineStation>& route);
        // Flatten the heights within the footprint; closedOutline (world XZ, a closed spline's
        // outline) is the Area region's inside, empty for Path. Returns the texels changed.
        static TerrainMapRegion bakeHeights(const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& closedOutline,
                                            const SplineTerrainSettings& settings, const TerrainBakeFrame& frame, TerrainBakeMap& map);
        // Paint layer channel (0-2) of this blend map toward full, the others toward none
        static TerrainMapRegion bakeBlend(const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& closedOutline,
                                          const SplineTerrainSettings& settings, const TerrainBakeFrame& frame, int channel,
                                          TerrainBakeMap& map);
        // Scale a foliage density map toward zero
        static TerrainMapRegion bakeDensity(const std::vector<spline::SplineStation>& route, const std::vector<Vector3>& closedOutline,
                                            const SplineTerrainSettings& settings, const TerrainBakeFrame& frame, TerrainBakeMap& map);

        // ---- in the editor

        // The terrain a spline bakes into: its settings' terrain, else the first terrain under it
        static Entity findTerrain(Scene* scene, Entity splineEntity);
        // Bakes the spline's settings into its terrain as one undo step (false and an error when
        // nothing could be baked: no terrain, no spline, no data)
        static bool apply(Project* project, uint32_t sceneId, Entity splineEntity, TerrainBakeChanges& changes, std::string& error);
        // The same into the maps without commands
        static bool bakeInPlace(Project* project, uint32_t sceneId, Entity splineEntity, Entity terrainEntity,
                                TerrainBakeChanges& changes, std::string& error);
        // The same into a store: an overlay over the terrain's maps is a step or a live session
        static bool bakeInto(terrain::TerrainMapStore& store, Project* project, uint32_t sceneId, Entity splineEntity, Entity terrainEntity,
                             TerrainBakeChanges& changes, std::string& error);
        // whether the terrain's heightmap is 8-bit (the bake warns: the bed steps visibly)
        static bool isHeightMap8Bit(Scene* scene, Entity terrainEntity);
    };

    // Live Terrain (S18 R12): while it is on during an Edit Spline session, the spline's bake runs on
    // an overlay over the terrain as the session found it (T1 R20): each spline change rewinds what
    // it touched and bakes again, so it holds only the tiles it has been over, and the session commits
    // one step when it ends. A later bake works on the terrain as it then is.
    class TerrainSplineLiveSession{
    private:
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity spline = NULL_ENTITY;
        Entity terrain = NULL_ENTITY;
        std::unique_ptr<EditorTerrainStore> store;
        std::unique_ptr<terrain::OverlayMapStore> overlay;
        uint64_t bakedVersion = 0;
        bool active = false;

    public:
        bool begin(Project* project, uint32_t sceneId, Entity splineEntity, std::string& error);
        // re-bakes when the spline changed since the last bake; true when it did
        bool update(std::string& error);
        // one step of everything the session changed ("Apply Spline to Terrain")
        void end();
        // puts the terrain back as the session found it and records nothing
        void cancel();
        bool isActive() const { return active; }
        Entity getSpline() const { return spline; }
        uint32_t getSceneId() const { return sceneId; }
        // the last re-bake's duration (the R20 budget)
        double lastBakeMs = 0.0;
    };

}
