// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelSliceRender.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // 1/256 voxel: finer than a pixel at the largest scale
    constexpr int SLICE_FIXED = 256;
    constexpr int SLICE_MAX_UPSCALE = 8;
    constexpr int SLICE_GRID = voxel::BLOCK_SIZE;

    constexpr VoxelSlotColor kSlotColors[voxel::MAX_SLOTS] = {
        {168, 142, 112}, {104, 158, 92}, {146, 150, 158}, {204, 166, 84},
        {92, 136, 188}, {186, 96, 84}, {134, 102, 170}, {88, 170, 160},
        {214, 200, 164}, {120, 92, 66}, {168, 196, 96}, {204, 120, 156},
        {72, 96, 128}, {226, 142, 72}, {108, 112, 88}, {196, 196, 204},
    };

    const uint8_t kIsoLine[3] = {250, 250, 250};
    const uint8_t kVolumeOutline[3] = {255, 196, 56};
    const uint8_t kTerrainOutline[3] = {96, 232, 112};

    void setSlicePixel(VoxelSliceImage& image, int x, int y, const uint8_t* rgb) {
        if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
        uint8_t* pixel = image.rgba.data() + 4 * (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x));
        pixel[0] = rgb[0];
        pixel[1] = rgb[1];
        pixel[2] = rgb[2];
        pixel[3] = 255;
    }

    // the plane's samples, with the store's own read: x, y and z of (column, row) in the plane
    voxel::Int3 planeSample(int axis, int position, int column, int row) {
        switch (axis) {
            case 0: return {position, row, column};
            case 1: return {column, position, row};
            default: return {column, row, position};
        }
    }

    // the length of the zero contour through one cell of four samples, in voxels
    double cellContourLength(int a, int b, int c, int d) {
        // corners: a (0,0), b (1,0), c (0,1), d (1,1); negative is solid
        const auto crossing = [](int v0, int v1) { return static_cast<double>(v0) / static_cast<double>(v0 - v1); };
        const bool sa = a < 0, sb = b < 0, sc = c < 0, sd = d < 0;
        double xs[4], ys[4];
        int points = 0;
        if (sa != sb) { xs[points] = crossing(a, b); ys[points++] = 0.0; }
        if (sc != sd) { xs[points] = crossing(c, d); ys[points++] = 1.0; }
        if (sa != sc) { xs[points] = 0.0; ys[points++] = crossing(a, c); }
        if (sb != sd) { xs[points] = 1.0; ys[points++] = crossing(b, d); }
        const auto segment = [&](int i, int j) {
            const double dx = xs[i] - xs[j], dy = ys[i] - ys[j];
            return std::sqrt(dx * dx + dy * dy);
        };
        if (points == 2) return segment(0, 1);
        if (points != 4) return 0.0;
        // the saddle: the centre's sign pairs the crossings; (a, c)-side corner keeps its corner
        const bool centreSolid = static_cast<int64_t>(a) + b + c + d < 0;
        // crossings in order: 0 bottom, 1 top, 2 left, 3 right
        const bool cornerAIsolated = centreSolid != sa;
        return cornerAIsolated ? segment(0, 2) + segment(1, 3) : segment(0, 3) + segment(1, 2);
    }

}

VoxelSlotColor editor::voxelSlotColor(int slot) {
    return kSlotColors[static_cast<size_t>(slot) & (voxel::MAX_SLOTS - 1)];
}

bool editor::renderVoxelSlice(voxel::Store& store, const VoxelSliceRequest& request, VoxelSliceImage& image) {
    image = VoxelSliceImage{};
    const voxel::Int3 samples = store.levelSamples(0);
    if (samples.x <= 0 || samples.y <= 0 || samples.z <= 0) return false;

    const int axis = static_cast<int>(request.axis);
    const int axisSamples = axis == 0 ? samples.x : (axis == 1 ? samples.y : samples.z);
    const int position = request.position < 0 ? axisSamples / 2 : std::min(request.position, axisSamples - 1);

    image.axis = request.axis;
    image.position = position;
    image.voxelSize = store.desc().voxelSize;
    // X looks along +x with z across, Z along +z with x across: y is up in both. Y is the map view, z down.
    image.columnAxis = axis == 0 ? 2 : 0;
    image.rowAxis = axis == 1 ? 2 : 1;
    image.rowFlipped = axis != 1;

    const int columns = image.columnAxis == 0 ? samples.x : samples.z;
    const int rows = image.rowAxis == 1 ? samples.y : samples.z;
    const int longer = std::max(columns, rows);
    const int maxWidth = std::max(request.maxWidth, 1);
    if (longer <= maxWidth) {
        image.scaleNum = std::min(maxWidth / longer, SLICE_MAX_UPSCALE);
        image.scaleDen = 1;
    } else {
        image.scaleNum = 1;
        image.scaleDen = (longer + maxWidth - 1) / maxWidth;
    }
    image.width = std::max(columns * image.scaleNum / image.scaleDen, 1);
    image.height = std::max(rows * image.scaleNum / image.scaleDen, 1);

    // the plane once: every later step reads these
    const size_t sampleCount = static_cast<size_t>(columns) * static_cast<size_t>(rows);
    std::vector<int16_t> sdf(sampleCount);
    std::vector<uint16_t> material(sampleCount);
    int64_t solid = 0;
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            const voxel::Int3 p = planeSample(axis, position, column, row);
            const size_t index = static_cast<size_t>(row) * static_cast<size_t>(columns) + static_cast<size_t>(column);
            sdf[index] = store.sdfAt(0, p.x, p.y, p.z);
            material[index] = store.materialAt(0, p.x, p.y, p.z);
            if (sdf[index] < 0) solid++;
        }
    }
    image.solidFraction = static_cast<double>(solid) / static_cast<double>(sampleCount);
    image.airFraction = 1.0 - image.solidFraction;

    double contour = 0.0;
    for (int row = 0; row + 1 < rows; row++) {
        const int16_t* line = sdf.data() + static_cast<size_t>(row) * static_cast<size_t>(columns);
        const int16_t* next = line + columns;
        for (int column = 0; column + 1 < columns; column++) {
            contour += cellContourLength(line[column], line[column + 1], next[column], next[column + 1]);
        }
    }
    image.isoLineLength = contour * static_cast<double>(store.desc().voxelSize);

    // Pixel centres in 1/256 voxel. Rounding down keeps it integer, so no platform's float rounding
    // reaches a byte (R70).
    const auto pixelToFixed = [&](int pixel, int count) {
        const int64_t fixed = (2 * static_cast<int64_t>(pixel) + 1) * image.scaleDen * SLICE_FIXED / (2 * image.scaleNum) - SLICE_FIXED / 2;
        return static_cast<int>(std::clamp<int64_t>(fixed, 0, static_cast<int64_t>(count - 1) * SLICE_FIXED));
    };
    std::vector<int> columnFixed(static_cast<size_t>(image.width));
    std::vector<int> rowFixed(static_cast<size_t>(image.height));
    for (int x = 0; x < image.width; x++) columnFixed[static_cast<size_t>(x)] = pixelToFixed(x, columns);
    for (int y = 0; y < image.height; y++) {
        const int row = image.rowFlipped ? image.height - 1 - y : y;
        rowFixed[static_cast<size_t>(y)] = pixelToFixed(row, rows);
    }

    image.rgba.resize(4 * static_cast<size_t>(image.width) * static_cast<size_t>(image.height));
    std::vector<int32_t> pixelSdf(static_cast<size_t>(image.width) * static_cast<size_t>(image.height));

    for (int y = 0; y < image.height; y++) {
        const int vFixed = rowFixed[static_cast<size_t>(y)];
        const int v0 = vFixed / SLICE_FIXED;
        const int v1 = std::min(v0 + 1, rows - 1);
        const int fv = vFixed - v0 * SLICE_FIXED;
        for (int x = 0; x < image.width; x++) {
            const int uFixed = columnFixed[static_cast<size_t>(x)];
            const int u0 = uFixed / SLICE_FIXED;
            const int u1 = std::min(u0 + 1, columns - 1);
            const int fu = uFixed - u0 * SLICE_FIXED;
            const size_t base0 = static_cast<size_t>(v0) * static_cast<size_t>(columns);
            const size_t base1 = static_cast<size_t>(v1) * static_cast<size_t>(columns);
            const int64_t top = int64_t(sdf[base0 + static_cast<size_t>(u0)]) * (SLICE_FIXED - fu) + int64_t(sdf[base0 + static_cast<size_t>(u1)]) * fu;
            const int64_t bottom = int64_t(sdf[base1 + static_cast<size_t>(u0)]) * (SLICE_FIXED - fu) + int64_t(sdf[base1 + static_cast<size_t>(u1)]) * fu;
            const int32_t value = static_cast<int32_t>((top * (SLICE_FIXED - fv) + bottom * fv) / (SLICE_FIXED * SLICE_FIXED));
            pixelSdf[static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)] = value;

            uint8_t rgb[3];
            if (value < 0) {
                // the nearest sample's slot, darker with depth: a wide wall reads as filled, not flat
                const int nearestU = fu * 2 >= SLICE_FIXED ? u1 : u0;
                const int nearestV = fv * 2 >= SLICE_FIXED ? v1 : v0;
                const uint16_t packed = material[static_cast<size_t>(nearestV) * static_cast<size_t>(columns) + static_cast<size_t>(nearestU)];
                const int slot = voxel::materialWeightB(packed) * 2 >= 255 ? voxel::materialSlotB(packed) : voxel::materialSlotA(packed);
                const VoxelSlotColor color = voxelSlotColor(slot);
                const int depth = std::min(-value, voxel::SDF_MAX);
                const int shade = 255 - depth * 150 / voxel::SDF_MAX;
                rgb[0] = static_cast<uint8_t>(color.r * shade / 255);
                rgb[1] = static_cast<uint8_t>(color.g * shade / 255);
                rgb[2] = static_cast<uint8_t>(color.b * shade / 255);
            } else {
                // air: lighter close to a surface
                const int near = voxel::SDF_MAX - std::min(value, voxel::SDF_MAX);
                rgb[0] = static_cast<uint8_t>(24 + near * 34 / voxel::SDF_MAX);
                rgb[1] = static_cast<uint8_t>(28 + near * 38 / voxel::SDF_MAX);
                rgb[2] = static_cast<uint8_t>(36 + near * 46 / voxel::SDF_MAX);
            }
            setSlicePixel(image, x, y, rgb);
        }
    }

    if (request.blockGrid) {
        const int gridFixed = SLICE_GRID * SLICE_FIXED;
        const auto dimPixel = [&](int x, int y) {
            uint8_t* pixel = image.rgba.data() + 4 * (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x));
            for (int c = 0; c < 3; c++) pixel[c] = static_cast<uint8_t>((pixel[c] * 3 + 160) / 4);
        };
        for (int x = 1; x < image.width; x++) {
            if (columnFixed[static_cast<size_t>(x)] / gridFixed == columnFixed[static_cast<size_t>(x) - 1] / gridFixed) continue;
            for (int y = 0; y < image.height; y++) dimPixel(x, y);
        }
        for (int y = 1; y < image.height; y++) {
            if (rowFixed[static_cast<size_t>(y)] / gridFixed == rowFixed[static_cast<size_t>(y) - 1] / gridFixed) continue;
            for (int x = 0; x < image.width; x++) dimPixel(x, y);
        }
    }

    // the iso-line: a solid pixel with an air pixel beside it
    for (int y = 0; y < image.height; y++) {
        const int32_t* line = pixelSdf.data() + static_cast<size_t>(y) * static_cast<size_t>(image.width);
        for (int x = 0; x < image.width; x++) {
            if (line[x] >= 0) continue;
            const bool edge = (x > 0 && line[x - 1] >= 0) || (x + 1 < image.width && line[x + 1] >= 0) ||
                              (y > 0 && line[x - image.width] >= 0) || (y + 1 < image.height && line[x + image.width] >= 0);
            if (edge) setSlicePixel(image, x, y, kIsoLine);
        }
    }

    if (request.terrainHeight) {
        const double voxelSize = static_cast<double>(store.desc().voxelSize);
        const double planeMetres = static_cast<double>(position) * voxelSize;
        if (image.rowAxis == 1) {
            // a vertical plane: the terrain is a line of heights, joined column to column
            bool hasLast = false;
            int lastRow = 0;
            for (int x = 0; x < image.width; x++) {
                const double across = static_cast<double>(columnFixed[static_cast<size_t>(x)]) / SLICE_FIXED * voxelSize;
                double height = 0.0;
                const bool has = axis == 0 ? request.terrainHeight(planeMetres, across, height) : request.terrainHeight(across, planeMetres, height);
                if (!has) {
                    hasLast = false;
                    continue;
                }
                const int voxelRow = static_cast<int>(std::floor(height / voxelSize * image.scaleNum / image.scaleDen));
                const int imageRow = image.height - 1 - voxelRow;
                const int from = hasLast ? std::min(lastRow, imageRow) : imageRow;
                const int to = hasLast ? std::max(lastRow, imageRow) : imageRow;
                for (int y = std::max(from, 0); y <= std::min(to, image.height - 1); y++) setSlicePixel(image, x, y, kTerrainOutline);
                hasLast = true;
                lastRow = imageRow;
            }
        } else {
            // the map view: where the terrain crosses the plane
            std::vector<int8_t> above(static_cast<size_t>(image.width) * static_cast<size_t>(image.height));
            for (int y = 0; y < image.height; y++) {
                const double z = static_cast<double>(rowFixed[static_cast<size_t>(y)]) / SLICE_FIXED * voxelSize;
                for (int x = 0; x < image.width; x++) {
                    const double across = static_cast<double>(columnFixed[static_cast<size_t>(x)]) / SLICE_FIXED * voxelSize;
                    double height = 0.0;
                    const bool has = request.terrainHeight(across, z, height);
                    above[static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)] = !has ? int8_t(-1) : (height >= planeMetres ? int8_t(1) : int8_t(0));
                }
            }
            for (int y = 0; y < image.height; y++) {
                const int8_t* line = above.data() + static_cast<size_t>(y) * static_cast<size_t>(image.width);
                for (int x = 0; x < image.width; x++) {
                    if (line[x] < 0) continue;
                    const bool right = x + 1 < image.width && line[x + 1] >= 0 && line[x + 1] != line[x];
                    const bool down = y + 1 < image.height && line[x + image.width] >= 0 && line[x + image.width] != line[x];
                    if (right || down) setSlicePixel(image, x, y, kTerrainOutline);
                }
            }
        }
    }

    for (int x = 0; x < image.width; x++) {
        setSlicePixel(image, x, 0, kVolumeOutline);
        setSlicePixel(image, x, image.height - 1, kVolumeOutline);
    }
    for (int y = 0; y < image.height; y++) {
        setSlicePixel(image, 0, y, kVolumeOutline);
        setSlicePixel(image, image.width - 1, y, kVolumeOutline);
    }
    return true;
}

voxel::Vec3d editor::voxelSlicePixelToLocal(const VoxelSliceImage& image, float pixelX, float pixelY) {
    const double perVoxel = static_cast<double>(image.scaleNum) / static_cast<double>(image.scaleDen);
    const double row = image.rowFlipped ? static_cast<double>(image.height) - static_cast<double>(pixelY) : static_cast<double>(pixelY);
    const double voxelSize = static_cast<double>(image.voxelSize);
    double coords[3] = {0.0, 0.0, 0.0};
    coords[image.columnAxis] = static_cast<double>(pixelX) / perVoxel * voxelSize;
    coords[image.rowAxis] = row / perVoxel * voxelSize;
    coords[static_cast<int>(image.axis)] = static_cast<double>(image.position) * voxelSize;
    return voxel::Vec3d{coords[0], coords[1], coords[2]};
}
