// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/FoliageCull.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::terraincore;

FoliageCellVisibility jasperengine::terraincore::classifyFoliageBox(const FoliageFrustum& frustum, const float boxMin[3], const float boxMax[3], float padding) {
    bool inside = true;
    const float center[3] = {(boxMin[0] + boxMax[0]) * 0.5f, (boxMin[1] + boxMax[1]) * 0.5f, (boxMin[2] + boxMax[2]) * 0.5f};
    const float half[3] = {(boxMax[0] - boxMin[0]) * 0.5f, (boxMax[1] - boxMin[1]) * 0.5f, (boxMax[2] - boxMin[2]) * 0.5f};
    for (int p = 0; p < frustum.count; p++) {
        const float* plane = frustum.planes[p];
        const float distance = plane[0] * center[0] + plane[1] * center[1] + plane[2] * center[2] + plane[3];
        const float reach = std::abs(plane[0]) * half[0] + std::abs(plane[1]) * half[1] + std::abs(plane[2]) * half[2];
        if (distance + reach < -padding) {
            return FoliageCellVisibility::Outside;
        }
        // an instance sticking out by padding must still be whole inside the plane to skip its test
        if (distance - reach < padding) {
            inside = false;
        }
    }
    return inside ? FoliageCellVisibility::Inside : FoliageCellVisibility::Intersecting;
}

unsigned int jasperengine::terraincore::selectFoliageLod(const FoliageLodTable& table, float distance, float worldScale) {
    if (!table.enabled || table.numLods < 2) {
        return 0;
    }
    const float allowed = table.threshold * (table.perUnit * std::max(distance, 0.0f) + table.constant) / std::max(table.bias, 0.001f);
    unsigned int level = 0;
    const unsigned int levels = std::min(table.numLods, static_cast<unsigned int>(FOLIAGE_MAX_LODS));
    for (unsigned int l = 1; l < levels; l++) {
        if (table.error[l] * worldScale > allowed) {
            break;
        }
        level = l;
    }
    return level;
}

bool jasperengine::terraincore::foliageRankDraws(uint8_t rank, float density) {
    // written on the bytes, so that the subset of a density is exactly a prefix of the next one's
    return (static_cast<float>(rank) + 1.0f) / 256.0f <= density;
}

float jasperengine::terraincore::foliageDensityFalloff(const FoliageDetailCull& cull, float distance) {
    if (cull.farDensity >= 1.0f || !(cull.falloffEnd > cull.falloffStart)) {
        return 1.0f;
    }
    if (distance <= cull.falloffStart) return 1.0f;
    if (distance >= cull.falloffEnd) return cull.farDensity;
    const float t = (distance - cull.falloffStart) / (cull.falloffEnd - cull.falloffStart);
    return 1.0f + (cull.farDensity - 1.0f) * t;
}

float jasperengine::terraincore::foliageFadeScale(const FoliageDetailCull& cull, float distance) {
    float share = 1.0f;
    if (cull.limitDistance && cull.drawDistance > cull.fadeStart) {
        share = std::clamp((cull.drawDistance - distance) / (cull.drawDistance - cull.fadeStart), 0.0f, 1.0f);
    }
    if (cull.minDistance > cull.nearFadeStart) {
        share *= std::clamp((distance - cull.nearFadeStart) / (cull.minDistance - cull.nearFadeStart), 0.0f, 1.0f);
    }
    return share;
}

void jasperengine::terraincore::foliageBoxDistancesXZ(const float eye[3], const float boxMin[3], const float boxMax[3], float& nearest, float& farthest) {
    const float dx = std::max({boxMin[0] - eye[0], 0.0f, eye[0] - boxMax[0]});
    const float dz = std::max({boxMin[2] - eye[2], 0.0f, eye[2] - boxMax[2]});
    nearest = std::sqrt(dx * dx + dz * dz);
    const float fx = std::max(std::abs(eye[0] - boxMin[0]), std::abs(eye[0] - boxMax[0]));
    const float fz = std::max(std::abs(eye[2] - boxMin[2]), std::abs(eye[2] - boxMax[2]));
    farthest = std::sqrt(fx * fx + fz * fz);
}

bool jasperengine::terraincore::foliageCellInDistance(const FoliageDetailCull& cull, const float boxMin[3], const float boxMax[3]) {
    float nearest, farthest;
    foliageBoxDistancesXZ(cull.eye, boxMin, boxMax, nearest, farthest);
    if (cull.limitDistance && nearest >= cull.drawDistance) return false;
    if (cull.minDistance > cull.nearFadeStart && farthest <= cull.nearFadeStart) return false;
    return true;
}

size_t jasperengine::terraincore::cullFoliageCell(const FoliageInstance* instances, size_t count, const FoliageDetailCull& cull, const FoliageFrustum& frustum,
                                              FoliageCellVisibility visibility, FoliageInstance* out, uint8_t* lodOut, uint64_t* planeTests) {
    if (visibility == FoliageCellVisibility::Outside) return 0;

    const bool testPlanes = visibility != FoliageCellVisibility::Inside;
    const bool thin = cull.density < 1.0f || cull.farDensity < 1.0f;
    const bool fades = (cull.limitDistance && cull.drawDistance > cull.fadeStart) || cull.minDistance > cull.nearFadeStart;
    const bool lods = cull.lod.enabled && cull.lod.numLods > 1;
    const float maxDistanceSquared = cull.limitDistance ? cull.drawDistance * cull.drawDistance : 0.0f;
    uint64_t tests = 0;
    size_t written = 0;

    for (size_t i = 0; i < count; i++) {
        const FoliageInstance& instance = instances[i];
        const float dx = instance.position[0] - cull.eye[0];
        const float dz = instance.position[2] - cull.eye[2];
        const float xzSquared = dx * dx + dz * dz;
        if (cull.limitDistance && xzSquared > maxDistanceSquared) {
            continue;
        }

        float distance = 0.0f;
        const bool needsDistance = thin || fades;
        if (needsDistance) {
            distance = std::sqrt(xzSquared);
            if (thin && !foliageRankDraws(instance.densityRank, cull.density * foliageDensityFalloff(cull, distance))) {
                continue;
            }
        }

        const float tall = instance.scale * (instance.heightRatio > 128 ? static_cast<float>(instance.heightRatio) / 128.0f : 1.0f);
        const float radius = cull.meshRadius * tall;
        // instances turn about their origin: the centre of the bounds moves with them, and the sphere
        // around the origin holds it wherever it turned to
        const float centerReach = std::sqrt(cull.meshCenter[0] * cull.meshCenter[0] + cull.meshCenter[1] * cull.meshCenter[1] + cull.meshCenter[2] * cull.meshCenter[2]) * tall;
        const float reach = radius + centerReach;

        if (testPlanes) {
            bool outside = false;
            for (int p = 0; p < frustum.count; p++) {
                const float* plane = frustum.planes[p];
                tests++;
                if (plane[0] * instance.position[0] + plane[1] * instance.position[1] + plane[2] * instance.position[2] + plane[3] < -reach) {
                    outside = true;
                    break;
                }
            }
            if (outside) continue;
        }

        float share = 1.0f;
        if (fades) {
            share = foliageFadeScale(cull, distance);
            if (share <= 0.0f) continue;
        }

        out[written] = instance;
        out[written].scale = instance.scale * share;
        if (lods) {
            const float ox = instance.position[0] - cull.lodOrigin[0];
            const float oy = instance.position[1] - cull.lodOrigin[1];
            const float oz = instance.position[2] - cull.lodOrigin[2];
            const float lodDistance = std::sqrt(ox * ox + oy * oy + oz * oz) * cull.terrainScale - radius * cull.terrainScale;
            lodOut[written] = static_cast<uint8_t>(selectFoliageLod(cull.lod, lodDistance, instance.scale * cull.terrainScale));
        } else {
            lodOut[written] = 0;
        }
        written++;
    }

    if (planeTests) *planeTests += tests;
    return written;
}

FoliageTreeBands jasperengine::terraincore::selectFoliageTreeBands(const FoliageTreeBandSettings& settings, float nearest, float farthest, bool allowMesh) {
    FoliageTreeBands bands;
    if (nearest >= settings.treeDistance) {
        return bands;
    }

    const bool meshInRange = !settings.hasImpostor || nearest < settings.billboardStart;
    if (meshInRange && allowMesh) {
        bands.mesh = true;
        if (settings.explicitLods > 0) {
            unsigned int lod = 0;
            for (unsigned int l = 0; l < settings.lodDistanceCount && l < settings.explicitLods; l++) {
                if (nearest >= settings.lodDistances[l]) lod = l + 1;
            }
            bands.meshLod = lod;
        } else {
            bands.meshLod = selectFoliageLod(settings.lod, nearest, settings.prototypeError);
        }
    }

    if (settings.hasImpostor) {
        // both in the fade band: the dither shows each instance as one or the other (F6)
        const bool past = farthest > settings.billboardStart - settings.fadeLength;
        bands.impostor = past || !allowMesh;
    }

    bands.visible = bands.mesh || bands.impostor;
    return bands;
}

float jasperengine::terraincore::foliageMeshFade(const FoliageTreeBandSettings& settings, float distance) {
    if (settings.hasImpostor) {
        if (settings.fadeLength <= 0.0f) return distance < settings.billboardStart ? 1.0f : 0.0f;
        return std::clamp((settings.billboardStart - distance) / settings.fadeLength, 0.0f, 1.0f);
    }
    // with no impostor, the last LOD fades out at Tree Distance
    if (settings.fadeLength <= 0.0f) return distance < settings.treeDistance ? 1.0f : 0.0f;
    return std::clamp((settings.treeDistance - distance) / settings.fadeLength, 0.0f, 1.0f);
}

float jasperengine::terraincore::foliageImpostorFade(const FoliageTreeBandSettings& settings, float distance) {
    if (!settings.hasImpostor) return 0.0f;
    if (settings.fadeLength <= 0.0f) return (distance >= settings.billboardStart && distance < settings.treeDistance) ? 1.0f : 0.0f;
    const float in = 1.0f - std::clamp((settings.billboardStart - distance) / settings.fadeLength, 0.0f, 1.0f);
    const float out = std::clamp((settings.treeDistance - distance) / settings.fadeLength, 0.0f, 1.0f);
    return in * out;
}

bool jasperengine::terraincore::applyFoliageCaps(std::vector<FoliageCapCandidate>& candidates, uint32_t maxInstances, uint32_t maxDraws, uint32_t drawsAlreadyUsed) {
    std::vector<uint32_t> byDistance(candidates.size());
    for (uint32_t i = 0; i < byDistance.size(); i++) byDistance[i] = i;
    std::sort(byDistance.begin(), byDistance.end(), [&](uint32_t a, uint32_t b) {
        if (candidates[a].distance != candidates[b].distance) return candidates[a].distance < candidates[b].distance;
        return candidates[a].order < candidates[b].order;
    });

    uint64_t instances = 0;
    uint64_t draws = drawsAlreadyUsed;
    bool dropped = false;
    bool full = false;
    for (uint32_t index : byDistance) {
        FoliageCapCandidate& candidate = candidates[index];
        // once one cell is over, every farther one goes: nearer ones keep what they had, and a
        // small far cell cannot slip in past a dropped nearer one
        if (full || instances + candidate.instances > maxInstances || draws + candidate.draws > maxDraws) {
            full = true;
            candidate.kept = false;
            dropped = dropped || candidate.instances > 0;
            continue;
        }
        candidate.kept = true;
        instances += candidate.instances;
        draws += candidate.draws;
    }
    return dropped;
}
