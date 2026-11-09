// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelOverlay.h"

#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::unordered_map<Entity, VoxelOverlay>& overlays() {
        static std::unordered_map<Entity, VoxelOverlay> map;
        return map;
    }

    uint64_t overlayRevision = 1;

}

void editor::setVoxelOverlay(Entity owner, VoxelOverlay overlay) {
    auto& map = overlays();
    const auto found = map.find(owner);
    const VoxelOverlay current = found == map.end() ? VoxelOverlay::None : found->second;
    if (current == overlay) return;
    if (overlay == VoxelOverlay::None) map.erase(found);
    else map[owner] = overlay;
    overlayRevision++;
}

VoxelOverlay editor::getVoxelOverlay(Entity owner) {
    const auto& map = overlays();
    const auto found = map.find(owner);
    return found == map.end() ? VoxelOverlay::None : found->second;
}

void editor::clearVoxelOverlays() {
    if (overlays().empty()) return;
    overlays().clear();
    overlayRevision++;
}

bool editor::anyVoxelOverlay() {
    return !overlays().empty();
}

uint64_t editor::voxelOverlayRevision() {
    return overlayRevision;
}

const char* editor::voxelOverlayName(VoxelOverlay overlay) {
    switch (overlay) {
        case VoxelOverlay::None: return "none";
        case VoxelOverlay::Wireframe: return "wireframe";
        case VoxelOverlay::LodColours: return "lod_colours";
        case VoxelOverlay::Blocks: return "blocks";
        case VoxelOverlay::Collision: return "collision";
        case VoxelOverlay::Holes: return "holes";
        case VoxelOverlay::DominantMaterial: return "dominant_material";
    }
    return "none";
}

const char* editor::voxelOverlayLabel(VoxelOverlay overlay) {
    switch (overlay) {
        case VoxelOverlay::None: return "None";
        case VoxelOverlay::Wireframe: return "Wireframe";
        case VoxelOverlay::LodColours: return "LOD Colours";
        case VoxelOverlay::Blocks: return "Blocks";
        case VoxelOverlay::Collision: return "Collision";
        case VoxelOverlay::Holes: return "Holes";
        case VoxelOverlay::DominantMaterial: return "Dominant Material";
    }
    return "None";
}

bool editor::parseVoxelOverlay(std::string_view text, VoxelOverlay& overlay) {
    for (int value = 0; value < VOXEL_OVERLAY_COUNT; value++) {
        const VoxelOverlay candidate = static_cast<VoxelOverlay>(value);
        if (text == voxelOverlayName(candidate)) {
            overlay = candidate;
            return true;
        }
    }
    return false;
}
