// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One plane of a Voxel Volume as an image (docs/specs/T6-voxel-volumes.md R63, R70): solid and air
// shading, the iso-line, slots as colour, the block grid, the volume's and the terrain's outlines.
// Drawn on the CPU from level 0 in integer arithmetic, so equal samples give equal bytes on every
// target and through every way a block can be held. The Slice view and MCP's visualize_voxels both
// draw through it.

#include "voxel/VoxelStore.h"
#include "voxel/VoxelTypes.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine::editor {

    enum class VoxelSliceAxis : uint8_t { X, Y, Z };

    struct VoxelSliceRequest {
        VoxelSliceAxis axis = VoxelSliceAxis::Y;
        // the plane, in voxels along the axis; below 0: the middle of the volume. Past the end clamps.
        int position = -1;
        // the image's longer side reaches at most this many pixels; at least 1
        int maxWidth = 512;
        bool blockGrid = true;
        // The terrain's surface at (x, z), metres in the volume's frame; false where it has none. Null
        // draws no terrain outline. Called in pixel order, so a deterministic one keeps the bytes so.
        std::function<bool(double x, double z, double& height)> terrainHeight;
    };

    struct VoxelSliceImage {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;       // row 0 on top, 4 bytes per pixel, opaque
        VoxelSliceAxis axis = VoxelSliceAxis::Y;
        int position = 0;                // the plane drawn, after clamping
        // which volume axis (0 x, 1 y, 2 z) runs along the image's columns and rows
        int columnAxis = 0;
        int rowAxis = 2;
        // true: row 0 is the row axis' high end (Y looks up in the image)
        bool rowFlipped = false;
        // pixels per voxel, as a fraction
        int scaleNum = 1;
        int scaleDen = 1;
        float voxelSize = 1.0f;
        double isoLineLength = 0.0;      // metres, marching squares over the plane's samples
        double solidFraction = 0.0;      // of the plane's samples
        double airFraction = 0.0;
    };

    // False, leaving `image` empty, when the store has no samples to draw (no store dimensions).
    // Main thread: reading a cold or generator block resolves it, as every read of the store does.
    bool renderVoxelSlice(voxel::Store& store, const VoxelSliceRequest& request, VoxelSliceImage& image);

    // Where a pixel of the image lies: metres in the volume's frame, on the plane
    voxel::Vec3d voxelSlicePixelToLocal(const VoxelSliceImage& image, float pixelX, float pixelY);

    // A slot's colour wherever slots are told apart by colour (R63, R64): fixed, so images repeat
    struct VoxelSlotColor {
        uint8_t r = 0, g = 0, b = 0;
    };
    VoxelSlotColor voxelSlotColor(int slot);

}
