// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TileAlphaTrace.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    // the unit steps in counter-clockwise order, so direction + 1 is a left turn
    constexpr int TRACE_STEP[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

    int64_t traceSignedArea(const std::vector<std::array<int32_t, 2>>& points){
        int64_t twice = 0;
        for (size_t i = 0; i < points.size(); i++){
            const auto& a = points[i];
            const auto& b = points[(i + 1) % points.size()];
            twice += (int64_t)a[0] * b[1] - (int64_t)b[0] * a[1];
        }
        return twice;
    }

    bool traceStartsBefore(const std::array<int32_t, 2>& a, const std::array<int32_t, 2>& b){
        return a[1] != b[1] ? a[1] < b[1] : a[0] < b[0];
    }

}

std::vector<TilePolygon> jasperengine::traceAlpha(const uint8_t* rgba, int imageW, int imageH, int x, int y, int w, int h, int threshold){
    std::vector<TilePolygon> result;
    if (!rgba || w <= 0 || h <= 0 || imageW <= 0 || imageH <= 0)
        return result;
    const auto solidAt = [&](int column, int row){
        if (column < 0 || row < 0 || column >= w || row >= h)
            return false;
        // row 0 is the tile's bottom row, the image's rows run downwards
        const int64_t imageX = (int64_t)x + column;
        const int64_t imageY = (int64_t)y + (h - 1 - row);
        if (imageX < 0 || imageY < 0 || imageX >= imageW || imageY >= imageH)
            return false;
        return (int)rgba[(size_t)(imageY * imageW + imageX) * 4 + 3] >= threshold;
    };

    // Every boundary edge of a solid pixel, directed with the solid on its left: bit d of a corner
    // is an edge leaving that corner in direction d
    const int cornersWide = w + 1;
    std::vector<uint8_t> edges((size_t)(w + 1) * (size_t)(h + 1), 0);
    const auto cornerIndex = [&](int cx, int cy){
        return (size_t)cy * (size_t)cornersWide + (size_t)cx;
    };
    for (int row = 0; row < h; row++){
        for (int column = 0; column < w; column++){
            if (!solidAt(column, row))
                continue;
            if (!solidAt(column, row - 1))
                edges[cornerIndex(column, row)] |= 1;
            if (!solidAt(column + 1, row))
                edges[cornerIndex(column + 1, row)] |= 2;
            if (!solidAt(column, row + 1))
                edges[cornerIndex(column + 1, row + 1)] |= 4;
            if (!solidAt(column - 1, row))
                edges[cornerIndex(column, row + 1)] |= 8;
        }
    }

    for (int cy = 0; cy <= h; cy++){
        for (int cx = 0; cx <= w; cx++){
            while (edges[cornerIndex(cx, cy)] != 0){
                int direction = 0;
                while ((edges[cornerIndex(cx, cy)] & (1 << direction)) == 0)
                    direction++;
                std::vector<std::array<int32_t, 2>> corners;
                int px = cx, py = cy;
                // Two out-edges meet only where solid pixels touch at a corner. Turning left
                // first stays on the pixel the edge came from, which keeps every loop simple.
                while (true){
                    edges[cornerIndex(px, py)] &= (uint8_t)~(1u << direction);
                    corners.push_back({px, py});
                    px += TRACE_STEP[direction][0];
                    py += TRACE_STEP[direction][1];
                    const uint8_t out = edges[cornerIndex(px, py)];
                    if (out == 0)
                        break;
                    int next = -1;
                    for (int turn : {1, 0, 3}){
                        const int candidate = (direction + turn) % 4;
                        if (out & (1 << candidate)){
                            next = candidate;
                            break;
                        }
                    }
                    if (next < 0)
                        break;
                    direction = next;
                }

                std::vector<std::array<int32_t, 2>> merged;
                const size_t count = corners.size();
                for (size_t i = 0; i < count; i++){
                    const auto& previous = corners[(i + count - 1) % count];
                    const auto& current = corners[i];
                    const auto& following = corners[(i + 1) % count];
                    const bool collinear = (current[0] - previous[0]) * (following[1] - current[1]) == (current[1] - previous[1]) * (following[0] - current[0]);
                    if (!collinear)
                        merged.push_back(current);
                }
                std::rotate(merged.begin(), std::min_element(merged.begin(), merged.end(), traceStartsBefore), merged.end());
                TilePolygon polygon;
                polygon.points = std::move(merged);
                result.push_back(std::move(polygon));
            }
        }
    }

    std::stable_sort(result.begin(), result.end(), [](const TilePolygon& a, const TilePolygon& b){
        if (a.points[0] != b.points[0])
            return traceStartsBefore(a.points[0], b.points[0]);
        return traceSignedArea(a.points) > 0 && traceSignedArea(b.points) < 0;
    });
    return result;
}
