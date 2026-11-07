// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainQuality.h"


#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    constexpr TerrainTierInfo TIERS[TERRAIN_QUALITY_COUNT] = {
        // HIGH: as before T3, every layer and every read; Limit Layers Per Pixel keeps the strongest 4
        {0, 4, 4, true, false, true, true, false, true, true, true, 2048, 0},
        // MEDIUM: 3 layers, 2 reads per map: 3 + 3 x 3 x 2
        {3, 3, 2, false, true, true, true, false, true, true, true, 2048, 21},
        // LOW: 2 layers, colour for both and the normal for the strongest: 3 + 2 + 1
        {2, 2, 1, false, true, false, false, true, false, false, false, 1024, 6},
    };

    constexpr const char* NAMES[TERRAIN_QUALITY_COUNT] = {"high", "medium", "low"};

    struct RuntimeState {
        TerrainQuality platform[TerrainQualitySettings::PLATFORM_COUNT] = {
            TerrainQuality::HIGH, TerrainQuality::HIGH, TerrainQuality::HIGH,
            TerrainQuality::HIGH, TerrainQuality::HIGH, TerrainQuality::HIGH};
        uint32_t shipped = TERRAIN_ALL_TIERS;
        bool hasOverride = false;
        TerrainQuality globalOverride = TerrainQuality::HIGH;
        bool hasPreview = false;
        TerrainQuality preview = TerrainQuality::HIGH;
        TerrainDebugView debugView = TerrainDebugView::SHADED;
        uint64_t revision = 1;
        bool clampWarned = false;
    };

    RuntimeState& state() {
        static RuntimeState value;
        return value;
    }

}

const TerrainTierInfo& jasperengine::terrain::terrainTierInfo(TerrainQuality quality) {
    return TIERS[static_cast<int>(quality)];
}

const char* jasperengine::terrain::terrainQualityName(TerrainQuality quality) {
    return NAMES[static_cast<int>(quality)];
}

bool jasperengine::terrain::terrainQualityFromName(const char* name, TerrainQuality& quality) {
    for (int i = 0; i < TERRAIN_QUALITY_COUNT; i++) {
        if (name && std::strcmp(name, NAMES[i]) == 0) {
            quality = static_cast<TerrainQuality>(i);
            return true;
        }
    }
    return false;
}

TerrainQuality jasperengine::terrain::clampTerrainQuality(TerrainQuality quality, uint32_t shippedMask) {
    if (shippedMask == 0 || (shippedMask & terrainQualityBit(quality))) return quality;
    // the nearest one, higher first: towards High, then down
    for (int i = static_cast<int>(quality) - 1; i >= 0; i--) {
        if (shippedMask & terrainQualityBit(static_cast<TerrainQuality>(i))) return static_cast<TerrainQuality>(i);
    }
    for (int i = static_cast<int>(quality) + 1; i < TERRAIN_QUALITY_COUNT; i++) {
        if (shippedMask & terrainQualityBit(static_cast<TerrainQuality>(i))) return static_cast<TerrainQuality>(i);
    }
    return quality;
}

TerrainQualityResolved jasperengine::terrain::resolveTerrainQuality(const TerrainQualityRequest& request) {
    TerrainQualityResolved resolved;
    if (request.hasPreview) {
        resolved.quality = request.preview;
        resolved.source = TerrainQualitySource::EDITOR_PREVIEW;
    } else if (request.hasOverride) {
        resolved.quality = request.globalOverride;
        resolved.source = TerrainQualitySource::GLOBAL_OVERRIDE;
    } else if (request.hasComponent) {
        resolved.quality = request.component;
        resolved.source = TerrainQualitySource::COMPONENT;
    } else {
        resolved.quality = request.project;
        resolved.source = TerrainQualitySource::PROJECT;
    }
    const TerrainQuality clamped = clampTerrainQuality(resolved.quality, request.shippedMask);
    resolved.clamped = clamped != resolved.quality;
    resolved.quality = clamped;
    return resolved;
}

void TerrainQualitySettings::setPlatformTier(int platform, TerrainQuality quality) {
    if (platform < 0 || platform >= PLATFORM_COUNT || state().platform[platform] == quality) return;
    state().platform[platform] = quality;
    state().revision++;
}

TerrainQuality TerrainQualitySettings::getPlatformTier(int platform) {
    return platform >= 0 && platform < PLATFORM_COUNT ? state().platform[platform] : TerrainQuality::HIGH;
}

void TerrainQualitySettings::setShippedMask(uint32_t mask) {
    if (state().shipped == mask) return;
    state().shipped = mask;
    state().revision++;
}

uint32_t TerrainQualitySettings::getShippedMask() {
    return state().shipped;
}

void TerrainQualitySettings::setGlobalOverride(TerrainQuality quality) {
    if (state().hasOverride && state().globalOverride == quality) return;
    state().hasOverride = true;
    state().globalOverride = quality;
    state().revision++;
}

void TerrainQualitySettings::clearGlobalOverride() {
    if (!state().hasOverride) return;
    state().hasOverride = false;
    state().revision++;
}

bool TerrainQualitySettings::getGlobalOverride(TerrainQuality& quality) {
    quality = state().globalOverride;
    return state().hasOverride;
}

void TerrainQualitySettings::setEditorPreview(TerrainQuality quality) {
    if (state().hasPreview && state().preview == quality) return;
    state().hasPreview = true;
    state().preview = quality;
    state().revision++;
}

void TerrainQualitySettings::clearEditorPreview() {
    if (!state().hasPreview) return;
    state().hasPreview = false;
    state().revision++;
}

bool TerrainQualitySettings::getEditorPreview(TerrainQuality& quality) {
    quality = state().preview;
    return state().hasPreview;
}

void TerrainQualitySettings::setDebugView(TerrainDebugView view) {
    if (state().debugView == view) return;
    state().debugView = view;
    state().revision++;
}

TerrainDebugView TerrainQualitySettings::getDebugView() {
    return state().debugView;
}

uint64_t TerrainQualitySettings::getRevision() {
    return state().revision;
}

bool TerrainQualitySettings::takeClampWarning() {
    if (state().clampWarned) return false;
    state().clampWarned = true;
    return true;
}

void TerrainQualitySettings::reset() {
    const uint64_t revision = state().revision;
    state() = RuntimeState();
    state().revision = revision + 1;
}
