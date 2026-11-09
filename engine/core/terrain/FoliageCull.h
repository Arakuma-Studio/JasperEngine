// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_CULL_H
#define FOLIAGE_CULL_H

// What a view keeps of the foliage (T5 R9, R10, R15, R21, F4-F6): pure over terrain-local instances,
// so it is tested without a GPU and its output does not depend on who ran it.

#include "Export.h"
#include "terrain/FoliageTypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::terraincore {

    inline constexpr int FOLIAGE_MAX_LODS = 4;

    // The six planes of a view in terrain-local space, normalized, inside positive: the far plane
    // can be left out (a camera with no far clip)
    struct FoliageFrustum {
        float planes[6][4] = {};
        int count = 6;
    };

    enum class FoliageCellVisibility : uint8_t { Outside, Intersecting, Inside };

    // Inside: no instance of the cell needs a plane test, given the padding every instance fits in
    JASPERENGINE_API FoliageCellVisibility classifyFoliageBox(const FoliageFrustum& frustum, const float boxMin[3], const float boxMax[3], float padding);

    // What RenderSystem::selectMeshLod needs: the same answer, without the render types
    struct FoliageLodTable {
        bool enabled = false;
        unsigned int numLods = 1;
        float error[FOLIAGE_MAX_LODS] = {};
        float threshold = 1.0f;
        float perUnit = 0.0f;
        float constant = 0.0f;
        float bias = 1.0f;
    };

    JASPERENGINE_API unsigned int selectFoliageLod(const FoliageLodTable& table, float distance, float worldScale);

    // XZ distance from the eye, for the fade and density (as the shader's fade measured it)
    struct FoliageDetailCull {
        float eye[3] = {0.0f, 0.0f, 0.0f};
        float lodOrigin[3] = {0.0f, 0.0f, 0.0f};
        float terrainScale = 1.0f;          // largest axis of the terrain's world scale

        bool limitDistance = true;          // off in a whole-terrain preview
        float drawDistance = 0.0f;          // the instance shrinks to nothing here
        float fadeStart = 0.0f;             // and starts to here (FOLIAGE_FADE_START of the distance)
        float minDistance = 0.0f;           // T5 R66: nearer than this it shrinks, too
        float nearFadeStart = 0.0f;

        float density = 1.0f;               // detailDensity x the layer's scale x the tier
        float falloffStart = 1.0f;          // distance where falloff leaves 1
        float falloffEnd = 1.0f;            // distance where it reaches farDensity
        float farDensity = 1.0f;

        float meshCenter[3] = {0.0f, 0.0f, 0.0f};
        float meshRadius = 0.0f;

        FoliageLodTable lod;
    };

    // F4's second step: the survivors of one cell, written from out, each with its LOD in lodOut.
    // visibility Inside skips the plane tests. Returns how many were written. planeTests counts them.
    JASPERENGINE_API size_t cullFoliageCell(const FoliageInstance* instances, size_t count, const FoliageDetailCull& cull, const FoliageFrustum& frustum,
                                            FoliageCellVisibility visibility, FoliageInstance* out, uint8_t* lodOut, uint64_t* planeTests = nullptr);

    // R10's rule, one instance
    JASPERENGINE_API bool foliageRankDraws(uint8_t rank, float density);
    JASPERENGINE_API float foliageDensityFalloff(const FoliageDetailCull& cull, float distance);
    // R10, R66: the share of its scale an instance keeps at this distance
    JASPERENGINE_API float foliageFadeScale(const FoliageDetailCull& cull, float distance);

    // Whether a whole cell can draw: the far fade, and a cell wholly nearer than the near fade's start
    JASPERENGINE_API bool foliageCellInDistance(const FoliageDetailCull& cull, const float boxMin[3], const float boxMax[3]);
    JASPERENGINE_API void foliageBoxDistancesXZ(const float eye[3], const float boxMin[3], const float boxMax[3], float& nearest, float& farthest);

    // ------------------------------------------------------------------------------- trees

    struct FoliageTreeBandSettings {
        float treeDistance = 1000.0f;
        float billboardStart = 80.0f;
        float fadeLength = 5.0f;
        bool hasImpostor = false;
        unsigned int explicitLods = 0;              // a mesh of its own per LOD
        float lodDistances[FOLIAGE_MAX_EXPLICIT_LODS] = {0.0f, 0.0f, 0.0f};  // LOD n + 1 starts here
        unsigned int lodDistanceCount = 0;
        FoliageLodTable lod;                        // the generated levels otherwise
        float prototypeError = 1.0f;                // largest instance scale in the cell x the terrain's
    };

    struct FoliageTreeBands {
        bool visible = false;
        bool mesh = false;
        unsigned int meshLod = 0;
        bool impostor = false;
    };

    // R15: from a cell's nearest and farthest points. allowMesh false (past Max Mesh Trees) leaves impostors only.
    JASPERENGINE_API FoliageTreeBands selectFoliageTreeBands(const FoliageTreeBandSettings& settings, float nearest, float farthest, bool allowMesh);

    // F6's factors for an instance's distance: 1 shown, 0 gone. The shader computes the same.
    JASPERENGINE_API float foliageMeshFade(const FoliageTreeBandSettings& settings, float distance);
    JASPERENGINE_API float foliageImpostorFade(const FoliageTreeBandSettings& settings, float distance);

    // ------------------------------------------------------------------------------- caps

    // One candidate of a view's budget: a detail cell or a tree cell, by the distance of its nearest point
    struct FoliageCapCandidate {
        float distance = 0.0f;
        uint32_t order = 0;     // ties: the canonical order, so a camera gives one result
        uint32_t instances = 0;
        uint32_t draws = 0;
        bool kept = false;
    };

    // R21: keeps the nearest within both caps and drops the farthest. Returns whether anything was dropped.
    JASPERENGINE_API bool applyFoliageCaps(std::vector<FoliageCapCandidate>& candidates, uint32_t maxInstances, uint32_t maxDraws, uint32_t drawsAlreadyUsed = 0);

}

#endif // FOLIAGE_CULL_H
