// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapChunkBuilder.h"

#include "math/Matrix4.h"
#include "pixelart/PixelSnap.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{

    // a tile's image in pixels from its bottom-left, y up, after its transform bits
    struct TileQuad{
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        double width = 0.0, height = 0.0;
    };

    TileQuad transformedQuad(const TileRenderInfo& tile, uint32_t flags){
        TileQuad q;
        q.x0 = tile.x0; q.y0 = tile.y0; q.x1 = tile.x1; q.y1 = tile.y1;
        q.width = tile.width;
        q.height = tile.height;
        // in image space (y down): transpose swaps the axes, then the flips mirror them
        if (flags & TILE_TRANSPOSE){
            const TileQuad o = q;
            q.width = o.height;
            q.height = o.width;
            q.x0 = o.height - o.y1;
            q.x1 = o.height - o.y0;
            q.y0 = o.width - o.x1;
            q.y1 = o.width - o.x0;
        }
        if (flags & TILE_FLIP_X){
            const double x0 = q.x0;
            q.x0 = q.width - q.x1;
            q.x1 = q.width - x0;
        }
        if (flags & TILE_FLIP_Y){
            const double y0 = q.y0;
            q.y0 = q.height - q.y1;
            q.y1 = q.height - y0;
        }
        return q;
    }

    // where a dual-grid layer's tile sits relative to its cell's anchor: half a cell down-left,
    // in cell space (R27), the same fractional cell on every layout
    void dualGridShift(const TileGridSpec& grid, double& sx, double& sy){
        if (grid.layout == TileGridLayout::Isometric){
            sx = 0.0;
            sy = -tilegrid::pitchY(grid) * 0.5;
        }else{
            sx = -tilegrid::pitchX(grid) * 0.5;
            sy = -tilegrid::pitchY(grid) * 0.5;
        }
    }

    struct PlacedCell{
        CellCoord cell;
        uint32_t value = 0;
        const TileRenderInfo* info = nullptr;
    };

    void writeInstance(const TilemapChunkParams& params, const PlacedCell& placed, InstanceRenderData& out, Vector2* pivot){
        const TileRenderInfo& tile = *placed.info;
        const uint32_t flags = cellValueFlags(placed.value);
        const double ppu = params.pixelsPerUnit;
        TilePoint anchor = tilegrid::cellToLocal(params.grid, placed.cell);
        anchor.x += params.tileAnchorX * tilegrid::pitchX(params.grid);
        anchor.y += params.tileAnchorY * tilegrid::pitchY(params.grid);
        if (params.dualGrid){
            double sx, sy;
            dualGridShift(params.grid, sx, sy);
            anchor.x += sx;
            anchor.y += sy;
        }
        const TileQuad quad = transformedQuad(tile, flags);
        // R8: the image's bottom-left at a rectangle's anchor, its bottom-centre at the others'
        double baseX = anchor.x * ppu;
        const double baseY = anchor.y * ppu + tile.offsetY;
        if (params.grid.layout != TileGridLayout::Rectangle)
            baseX -= quad.width * 0.5;
        baseX += tile.offsetX;
        double left = baseX + quad.x0, right = baseX + quad.x1;
        double bottom = baseY + quad.y0, top = baseY + quad.y1;
        if (params.snapEdges){
            left = pixelart::snap((float)left);
            right = pixelart::snap((float)right);
            bottom = pixelart::snap((float)bottom);
            top = pixelart::snap((float)top);
        }
        const float x0 = (float)(left / ppu), x1 = (float)(right / ppu);
        const float y0 = (float)(bottom / ppu), y1 = (float)(top / ppu);
        const float w = x1 - x0, h = y1 - y0;

        Matrix4 m;
        const bool xz = params.grid.plane == TileGridPlane::XZ;
        // the quad's axes in the plane; transpose swaps them (mirrored: no culling on the quad)
        float axisXu, axisXv, axisYu, axisYv, ou, ov;
        if (flags & TILE_TRANSPOSE){
            axisXu = 0.0f;  axisXv = -h;
            axisYu = -w;    axisYv = 0.0f;
            ou = x1; ov = y1;
        }else{
            axisXu = w;     axisXv = 0.0f;
            axisYu = 0.0f;  axisYv = h;
            ou = x0; ov = y0;
        }
        m[0][0] = axisXu; m[0][1] = xz ? 0.0f : axisXv; m[0][2] = xz ? -axisXv : 0.0f; m[0][3] = 0.0f;
        m[1][0] = axisYu; m[1][1] = xz ? 0.0f : axisYv; m[1][2] = xz ? -axisYv : 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = xz ? 1.0f : 0.0f; m[2][2] = xz ? 0.0f : 1.0f; m[2][3] = 0.0f;
        m[3][0] = ou; m[3][1] = xz ? 0.0f : ov; m[3][2] = xz ? -ov : 0.0f; m[3][3] = 1.0f;
        out.instanceMatrix = m;
        out.color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        out.textureRect = tilechunk::tileUvRect(tile, flags);
        if (pivot)
            *pivot = Vector2(ou, ov);
    }

    constexpr int64_t kNoFrameChange = INT64_MAX;

    int64_t instanceStartMs(const TilemapChunkParams& params, const TileAnimationRuntime& animation, CellCoord cell){
        if (!animation.randomStart || animation.totalMs <= 0)
            return 0;
        return (int64_t)(tileset::cellHash(params.seed, 0, cell.x, cell.y) % (uint32_t)animation.totalMs);
    }

    // the animation's frame at `ms` and when it next changes
    int animationFrameAt(const TileAnimationRuntime& animation, int64_t ms, int64_t& nextChange){
        const int64_t t = ((ms % animation.totalMs) + animation.totalMs) % animation.totalMs;
        int frame = 0;
        while (frame + 1 < (int)animation.endsMs.size() && t >= animation.endsMs[(size_t)frame])
            frame++;
        nextChange = ms + (animation.endsMs[(size_t)frame] - t);
        return frame;
    }

}

Rect tilechunk::tileUvRect(const TileRenderInfo& tile, uint32_t flags){
    float u0 = tile.u0, v0 = tile.v0, u1 = tile.u1, v1 = tile.v1;
    bool mirrorU = (flags & TILE_FLIP_X) != 0;
    bool mirrorV = (flags & TILE_FLIP_Y) != 0;
    if (flags & TILE_TRANSPOSE)
        std::swap(mirrorU, mirrorV);
    if (mirrorU)
        std::swap(u0, u1);
    if (mirrorV)
        std::swap(v0, v1);
    return Rect(u0, v0, u1 - u0, v1 - v0);
}

AABB tilechunk::chunkBounds(const TileGridSpec& grid, const CellRect& rect, double overhangX, double overhangY, double dualShiftX, double dualShiftY){
    TilePoint min{1e300, 1e300}, max{-1e300, -1e300};
    const CellCoord corners[4] = {{rect.minX, rect.minY}, {rect.maxX, rect.minY}, {rect.minX, rect.maxY}, {rect.maxX, rect.maxY}};
    for (const CellCoord& corner : corners){
        TilePoint lo, hi;
        tilegrid::cellBounds(grid, corner, lo, hi);
        min.x = std::min(min.x, lo.x); min.y = std::min(min.y, lo.y);
        max.x = std::max(max.x, hi.x); max.y = std::max(max.y, hi.y);
    }
    // the stagger and the diamond's corners sit a pitch beyond the rectangle's four cells at most
    if (grid.layout != TileGridLayout::Rectangle){
        const double px = tilegrid::pitchX(grid), py = tilegrid::pitchY(grid);
        min.x -= px; min.y -= py;
        max.x += px; max.y += py;
    }
    min.x += std::min(dualShiftX, 0.0) - overhangX;
    min.y += std::min(dualShiftY, 0.0) - overhangY;
    max.x += std::max(dualShiftX, 0.0) + overhangX;
    max.y += std::max(dualShiftY, 0.0) + overhangY;
    if (grid.plane == TileGridPlane::XZ)
        return AABB(Vector3((float)min.x, 0.0f, (float)-max.y), Vector3((float)max.x, 0.0f, (float)-min.y));
    return AABB(Vector3((float)min.x, (float)min.y, 0.0f), Vector3((float)max.x, (float)max.y, 0.0f));
}

void tilechunk::build(const TilemapChunkParams& params, ChunkCoord coord, SpriteInstanceBatch& batch, TilemapChunkBuild& out){
    out.animated.clear();
    out.runs.clear();
    out.rows.clear();
    out.nextFrameMs = kNoFrameChange;
    std::vector<PlacedCell> placed;
    const TileChunk* chunk = params.cells->chunk(coord);
    if (chunk){
        placed.reserve(chunk->nonEmpty);
        const int size = params.cells->getChunkSize();
        const CellCoord origin = params.cells->chunkOrigin(coord);
        for (int i = 0; i < size * size; i++){
            const uint32_t value = chunk->cells[(size_t)i];
            if (cellValueEmpty(value))
                continue;
            const TileRenderInfo* info = params.tileset->tile(cellValueTile(value));
            if (!info)
                continue;                // R90: a missing id draws nothing and keeps its cell
            placed.push_back({CellCoord{origin.x + i % size, origin.y + i / size}, value, info});
        }
    }
    // R32: where tiles may overlap the painter's order wins; otherwise one draw per page
    bool overlap = params.individual || tilegrid::layoutOverlaps(params.grid);
    const double ppu = params.pixelsPerUnit;
    if (!overlap){
        const double cellW = params.grid.cellWidth * ppu, cellH = params.grid.cellHeight * ppu;
        for (const PlacedCell& p : placed){
            if (p.info->width > cellW || p.info->height > cellH || p.info->offsetX != 0 || p.info->offsetY != 0){
                overlap = true;
                break;
            }
        }
    }
    if (overlap){
        std::sort(placed.begin(), placed.end(), [&](const PlacedCell& a, const PlacedCell& b){
            return tilegrid::painterLess(params.grid, a.cell, b.cell);
        });
    }else{
        std::stable_sort(placed.begin(), placed.end(), [](const PlacedCell& a, const PlacedCell& b){
            return a.info->page < b.info->page;
        });
    }

    batch.setCount((uint32_t)placed.size());
    std::span<InstanceRenderData> instances = batch.instances();
    std::span<Vector2> pivots = batch.pivots();
    std::span<Vector4> fx = batch.fx();
    for (size_t n = 0; n < placed.size(); n++){
        PlacedCell shown = placed[n];
        const int32_t tileId = cellValueTile(shown.value);
        if (shown.info->animation >= 0){
            const TileAnimationRuntime& animation = params.tileset->animations[(size_t)shown.info->animation];
            if (animation.totalMs > 0){
                TilemapAnimatedInstance entry;
                entry.instance = (uint32_t)n;
                entry.tile = tileId;
                entry.flags = cellValueFlags(shown.value);
                entry.startMs = instanceStartMs(params, animation, shown.cell);
                out.animated.push_back(entry);
                int64_t next = kNoFrameChange;
                const int frame = animationFrameAt(animation, params.animationMs + entry.startMs, next);
                out.nextFrameMs = std::min(out.nextFrameMs, next - entry.startMs);
                if (const TileRenderInfo* current = params.tileset->tile(animation.frames[(size_t)frame]))
                    shown.info = current;
            }
        }
        writeInstance(params, shown, instances[n], pivots.empty() ? nullptr : &pivots[n]);
        if (params.individual){
            const TilePoint anchor = tilegrid::cellToLocal(params.grid, shown.cell);
            if (out.rows.empty() || out.rows.back().anchorV != anchor.y)
                out.rows.push_back({(uint32_t)n, 0u, anchor.x, anchor.y});
            out.rows.back().count++;
        }
        if (!fx.empty())
            fx[n] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        if (!out.runs.empty() && out.runs.back().page == shown.info->page)
            out.runs.back().count++;
        else
            out.runs.push_back({(uint32_t)n, 1u, shown.info->page});
    }
    batch.setPageRuns(out.runs);
    batch.markChanged(0.0f, 0.0f, 1.0f, 1.0f);
}

bool tilechunk::updateAnimation(const TilemapChunkParams& params, SpriteInstanceBatch& batch, TilemapChunkBuild& build, int64_t ms, bool& changed){
    changed = false;
    if (ms < build.nextFrameMs)
        return true;
    int64_t earliest = kNoFrameChange;
    std::span<InstanceRenderData> instances = batch.instances();
    for (const TilemapAnimatedInstance& entry : build.animated){
        const TileRenderInfo* still = params.tileset->tile(entry.tile);
        if (!still || still->animation < 0 || entry.instance >= instances.size())
            return false;
        const TileAnimationRuntime& animation = params.tileset->animations[(size_t)still->animation];
        int64_t next = kNoFrameChange;
        const int frame = animationFrameAt(animation, ms + entry.startMs, next);
        earliest = std::min(earliest, next - entry.startMs);
        const TileRenderInfo* current = params.tileset->tile(animation.frames[(size_t)frame]);
        if (!current)
            return false;
        const Rect rect = tileUvRect(*current, entry.flags);
        InstanceRenderData& instance = instances[entry.instance];
        if (instance.textureRect.getX() == rect.getX() && instance.textureRect.getY() == rect.getY() &&
                instance.textureRect.getWidth() == rect.getWidth() && instance.textureRect.getHeight() == rect.getHeight())
            continue;
        // a frame on another page belongs to another run: rebuild (rare: an animation across pages)
        for (const SpritePageRun& run : batch.getPageRuns()){
            if (entry.instance >= run.first && entry.instance < run.first + run.count && run.page != current->page)
                return false;
        }
        instance.textureRect = rect;
        changed = true;
    }
    build.nextFrameMs = earliest;
    if (changed)
        batch.markChanged(0.0f, 0.0f, 1.0f, 1.0f);
    return true;
}
