// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapComposite.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace jasperengine::editor::tilemap {

    namespace {

        PixelProblem compositeProblem(const char* kind, std::string message, std::string fix, std::string field = {}) {
            PixelProblem p;
            p.kind = kind;
            p.field = std::move(field);
            p.message = std::move(message);
            p.fix = std::move(fix);
            return p;
        }

        // floor(v + 0.5): positions on half pixels (a bottom-centre anchor of an odd-width tile)
        // always round the same way, so identical cells give identical bytes
        int compositeRound(double value) {
            return (int)std::floor(value + 0.5);
        }

        struct CompositeCell {
            CellCoord cell;
            uint32_t value = 0;
        };

        // the tile as the cell shows it, cached by (tile, transform bits): the tiles repeat a lot
        const PixelImage* compositeTileImage(const TilesetRuntime& tileset, TilesetPagePixels& pages, uint32_t value,
                                             std::map<uint32_t, PixelImage>& cache, std::map<int32_t, PixelImage>& base) {
            const uint32_t key = value & (TILE_ID_MASK | TILE_TRANSFORM_MASK);
            const auto found = cache.find(key);
            if (found != cache.end()) return &found->second;
            const int32_t tile = cellValueTile(value);
            auto source = base.find(tile);
            if (source == base.end()) {
                PixelImage image;
                if (!tileImage(tileset, tile, pages, image)) return nullptr;
                source = base.emplace(tile, std::move(image)).first;
            }
            return &cache.emplace(key, transformTileImage(source->second, cellValueFlags(value))).first->second;
        }

    }

    PixelImage transformTileImage(const PixelImage& image, uint32_t flags) {
        if ((flags & TILE_TRANSFORM_MASK) == 0) return image;
        const bool transpose = (flags & TILE_TRANSPOSE) != 0;
        const int width = transpose ? image.height() : image.width();
        const int height = transpose ? image.width() : image.height();
        PixelImage out(width, height);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // in reverse order of the transform: where this output pixel comes from
                int sx = x, sy = y;
                if (flags & TILE_FLIP_Y) sy = height - 1 - sy;
                if (flags & TILE_FLIP_X) sx = width - 1 - sx;
                if (transpose) std::swap(sx, sy);
                out.set(x, y, image.at(sx, sy));
            }
        }
        return out;
    }

    bool compositeLayer(const CompositeParams& params, TilesetPagePixels& pages, CompositeResult& out, PixelProblem& error) {
        out = CompositeResult();
        if (params.scale < 1 || params.scale > 8) {
            error = compositeProblem("parse", "scale must be an integer from 1 to 8", "use a scale from 1 to 8", "scale");
            return false;
        }
        if (!params.cells || !params.tileset) {
            error = compositeProblem("tileset_missing", "the layer has no resolved tileset", "assign a Tileset to the Tilemap Layer", "tileset");
            return false;
        }
        if (params.region.isEmpty()) return true;
        if (params.region.exceeds(MAX_TILE_REGION_CELLS)) {
            error = compositeProblem("region_too_large", "the region has " + std::to_string(params.region.area()) + " cells, past the limit of " + std::to_string(MAX_TILE_REGION_CELLS),
                                     "ask for a smaller region", "region");
            return false;
        }
        const TileGridSpec& grid = params.grid;
        const double ppu = params.pixelsPerUnit > 0.0 ? params.pixelsPerUnit : 1.0;
        // the region's frame: its cells where the tiles of a dual grid sit; the tiles: tileShift
        const TilePoint frameShift = params.dualGrid ? tilegrid::dualGridShift(grid) : TilePoint{0.0, 0.0};
        const double shiftX = frameShift.x, shiftY = frameShift.y;
        const TilePoint tileShift = tilegrid::tileShift(grid, params.tileAnchorX, params.tileAnchorY, params.dualGrid);

        std::vector<CompositeCell> cells;
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        const CellRect& region = params.region;
        for (int64_t y = region.minY; y <= region.maxY; y++) {
            for (int64_t x = region.minX; x <= region.maxX; x++) {
                const CellCoord cell{(int32_t)x, (int32_t)y};
                TilePoint lo, hi;
                tilegrid::cellBounds(grid, cell, lo, hi);
                minX = std::min(minX, lo.x + shiftX);
                minY = std::min(minY, lo.y + shiftY);
                maxX = std::max(maxX, hi.x + shiftX);
                maxY = std::max(maxY, hi.y + shiftY);
                const uint32_t value = params.cells->get(cell);
                if (!cellValueEmpty(value)) cells.push_back(CompositeCell{cell, value});
            }
        }
        std::sort(cells.begin(), cells.end(), [&](const CompositeCell& a, const CompositeCell& b) {
            return tilegrid::painterLess(grid, a.cell, b.cell);
        });

        struct Placed {
            const PixelImage* image;
            int left;
            int bottom;
        };
        std::vector<Placed> placed;
        placed.reserve(cells.size());
        std::map<uint32_t, PixelImage> transformed;
        std::map<int32_t, PixelImage> baseImages;
        int leftEdge = compositeRound(minX * ppu), rightEdge = compositeRound(maxX * ppu);
        int bottomEdge = compositeRound(minY * ppu), topEdge = compositeRound(maxY * ppu);
        for (const CompositeCell& cell : cells) {
            const TileRenderInfo* info = params.tileset->tile(cellValueTile(cell.value));
            const PixelImage* image = info ? compositeTileImage(*params.tileset, pages, cell.value, transformed, baseImages) : nullptr;
            if (!image) {
                out.missing++;
                continue;
            }
            TilePoint anchor = tilegrid::cellToLocal(grid, cell.cell);
            anchor.x += tileShift.x;
            anchor.y += tileShift.y;
            // R8: the image's bottom-left at a rectangle's anchor, its bottom-centre at the others'
            double baseX = anchor.x * ppu;
            if (grid.layout != TileGridLayout::Rectangle) baseX -= image->width() * 0.5;
            baseX += info->offsetX;
            const double baseY = anchor.y * ppu + info->offsetY;
            const int left = compositeRound(baseX);
            const int bottom = compositeRound(baseY);
            placed.push_back(Placed{image, left, bottom});
            leftEdge = std::min(leftEdge, left);
            bottomEdge = std::min(bottomEdge, bottom);
            rightEdge = std::max(rightEdge, left + image->width());
            topEdge = std::max(topEdge, bottom + image->height());
        }

        const int64_t width = (int64_t)rightEdge - leftEdge, height = (int64_t)topEdge - bottomEdge;
        if (width <= 0 || height <= 0) return true;
        // by area, not only per side: a 1,000,000-cell region of 16-pixel tiles is a 256-megapixel image
        const int64_t scaledWidth = width * params.scale, scaledHeight = height * params.scale;
        if (scaledWidth > PixelImage::kMaxSide || scaledHeight > PixelImage::kMaxSide || scaledWidth * scaledHeight > MAX_COMPOSITE_PIXELS) {
            error = compositeProblem("region_too_large", "the image would be " + std::to_string(scaledWidth) + " x " + std::to_string(scaledHeight) +
                                     " pixels, past the limit of " + std::to_string(MAX_COMPOSITE_PIXELS) + " pixels",
                                     "pass a smaller region, or a lower scale", "region");
            return false;
        }
        PixelImage image((int)width, (int)height);
        for (const Placed& tile : placed) {
            image.blendOver(*tile.image, tile.left - leftEdge, topEdge - (tile.bottom + tile.image->height()), 255);
        }
        out.image = scaleNearest(image, params.scale);
        out.originX = leftEdge;
        out.originY = topEdge;
        return true;
    }

}
