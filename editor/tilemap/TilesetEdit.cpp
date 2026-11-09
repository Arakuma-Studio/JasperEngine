// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilesetEdit.h"

#include "Project.h"
#include "asset/ImportedAssets.h"
#include "io/FileData.h"
#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/aseprite/AsepriteImport.h"
#include "sprite/SpriteSheetBuilder.h"
#include "sprite/SpriteSheetPool.h"
#include "tilemap/TileAlphaTrace.h"
#include "util/ProjectFileWrites.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace jasperengine::editor::tilemap {

    namespace {

        PixelProblem tilesetEditProblem(const char* kind, std::string message, std::string fix, std::string field = {}) {
            PixelProblem p;
            p.kind = kind;
            p.field = std::move(field);
            p.message = std::move(message);
            p.fix = std::move(fix);
            return p;
        }

        struct SliceGrid {
            int columns = 0;
            int rows = 0;
        };

        bool sliceGridOf(const TextureSlicing& slicing, const PixelImage& image, SliceGrid& grid, PixelProblem& error) {
            if (slicing.tileWidth <= 0 || slicing.tileHeight <= 0 || slicing.margin < 0 || slicing.spacing < 0) {
                error = tilesetEditProblem("parse", "tile_size must be positive, margin and spacing at least 0", "give a tile size above 0", "tile_size");
                return false;
            }
            if (image.empty()) {
                error = tilesetEditProblem("tileset_missing", "the texture has no pixels", "use a texture that loads", "texture");
                return false;
            }
            const int stepX = slicing.tileWidth + slicing.spacing;
            const int stepY = slicing.tileHeight + slicing.spacing;
            grid.columns = (image.width() - 2 * slicing.margin + slicing.spacing) / stepX;
            grid.rows = (image.height() - 2 * slicing.margin + slicing.spacing) / stepY;
            if (grid.columns <= 0 || grid.rows <= 0) {
                error = tilesetEditProblem("parse", "the texture is too small for one tile of this size", "use a smaller tile size or margin", "tile_size");
                return false;
            }
            return true;
        }

        bool sliceIsEmpty(const PixelImage& image, int x, int y, int w, int h) {
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < w; i++) {
                    if (image.at(x + i, y + j)[3] != 0) return false;
                }
            }
            return true;
        }

        int nextSourceId(const TilesetData& data) {
            int id = 0;
            for (const TilesetSource& source : data.sources) id = std::max(id, source.id + 1);
            return id;
        }

        TilesetSource& addTextureSourceEntry(TilesetData& data, const AssetRef& texture, const TextureSlicing& slicing) {
            TilesetSource source;
            source.id = nextSourceId(data);
            source.kind = TilesetSourceKind::Texture;
            source.asset = texture;
            source.tileWidth = slicing.tileWidth;
            source.tileHeight = slicing.tileHeight;
            source.margin = slicing.margin;
            source.spacing = slicing.spacing;
            source.skipEmpty = slicing.skipEmpty;
            data.sources.push_back(std::move(source));
            return data.sources.back();
        }

        void sliceAddTiles(TilesetData& data, int sourceId, const TextureSlicing& slicing, const PixelImage& image, const SliceGrid& grid) {
            const int stepX = slicing.tileWidth + slicing.spacing, stepY = slicing.tileHeight + slicing.spacing;
            for (int row = 0; row < grid.rows; row++) {
                for (int column = 0; column < grid.columns; column++) {
                    const int x = slicing.margin + column * stepX, y = slicing.margin + row * stepY;
                    if (slicing.skipEmpty && sliceIsEmpty(image, x, y, slicing.tileWidth, slicing.tileHeight)) continue;
                    data.addTile(sourceId, column, row);
                }
            }
        }

        bool asepriteHexColor(const std::string& text, Vector4& out) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (text.size() != 9 || text[0] != '#' || std::sscanf(text.c_str() + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4) return false;
            out = Vector4((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
            return true;
        }

        // a user data value on its tile, its schema entry declared when new (R16: undeclared
        // properties are refused at write time)
        void declareTileProperty(TilesetData& data, TileDef& tile, const std::string& name, TilePropertyValue value, std::vector<std::string>& warnings) {
            if (const PropertySchemaEntry* entry = data.findProperty(name)) {
                if (entry->type != value.type) {
                    warnings.push_back("tile " + std::to_string(tile.id) + "'s user data '" + name + "' changes type and is skipped");
                    return;
                }
            } else {
                PropertySchemaEntry created;
                created.name = name;
                created.type = value.type;
                created.defaultValue.type = value.type;
                data.propertySchema.push_back(std::move(created));
            }
            tile.properties[name] = std::move(value);
        }

        void readAsepriteUserData(TilesetData& data, TileDef& tile, const nlohmann::json& userData, std::vector<std::string>& warnings) {
            if (!userData.is_object()) return;
            if (userData.contains("text") && userData["text"].is_string()) {
                TilePropertyValue value;
                value.type = TilePropertyType::String;
                value.s = userData["text"].get<std::string>();
                declareTileProperty(data, tile, "text", std::move(value), warnings);
            }
            if (userData.contains("color") && userData["color"].is_string()) {
                TilePropertyValue value;
                value.type = TilePropertyType::Color;
                if (asepriteHexColor(userData["color"].get<std::string>(), value.color)) declareTileProperty(data, tile, "color", std::move(value), warnings);
            }
            if (!userData.contains("properties") || !userData["properties"].is_object()) return;
            for (const auto& [name, item] : userData["properties"].items()) {
                TilePropertyValue value;
                if (item.is_boolean()) {
                    value.type = TilePropertyType::Bool;
                    value.b = item.get<bool>();
                } else if (item.is_number_integer() || item.is_number_unsigned()) {
                    value.type = TilePropertyType::Int;
                    value.i = item.get<int64_t>();
                } else if (item.is_number_float()) {
                    value.type = TilePropertyType::Float;
                    value.f = item.get<double>();
                } else if (item.is_string()) {
                    value.type = TilePropertyType::String;
                    value.s = item.get<std::string>();
                } else {
                    warnings.push_back("tile " + std::to_string(tile.id) + "'s user data '" + name + "' is not a simple value and is skipped");
                    continue;
                }
                declareTileProperty(data, tile, name, std::move(value), warnings);
            }
        }

        // a 3x5 digit font: one row per entry, top first, bit 2 the leftmost column
        constexpr uint8_t kContactDigits[10][5] = {
            {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
            {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
        };

        void contactDrawDigits(PixelImage& image, int x, int y, const std::string& digits, pixelart::PixelRgba color) {
            for (size_t d = 0; d < digits.size(); d++) {
                const uint8_t* glyph = kContactDigits[digits[d] - '0'];
                for (int row = 0; row < 5; row++) {
                    for (int column = 0; column < 3; column++) {
                        if (glyph[row] & (4 >> column)) image.set(x + (int)d * 4 + column, y + row, color);
                    }
                }
            }
        }

        pixelart::PixelRgba coverageColor(const std::string& hex) {
            unsigned r = 255, g = 255, b = 255;
            if (hex.size() == 7 && hex[0] == '#') std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
            return {(uint8_t)r, (uint8_t)g, (uint8_t)b, 255};
        }

        // which cell of the 3x3 a peering slot shows; (-1, -1) for a slot without one
        void coverageCellOfSlot(TileShape shape, int slot, int& column, int& row) {
            column = row = -1;
            if (shape == TileShape::HexPointy || shape == TileShape::HexFlat) {
                static constexpr int kPointy[6][2] = {{2, 1}, {2, 0}, {0, 0}, {0, 1}, {0, 2}, {2, 2}};
                static constexpr int kFlat[6][2] = {{2, 0}, {1, 0}, {0, 0}, {0, 2}, {1, 2}, {2, 2}};
                if (slot < 0 || slot >= 6) return;
                column = shape == TileShape::HexPointy ? kPointy[slot][0] : kFlat[slot][0];
                row = shape == TileShape::HexPointy ? kPointy[slot][1] : kFlat[slot][1];
                return;
            }
            static constexpr int kSquare[8][2] = {{1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}, {0, 0}};
            if (slot < 0 || slot >= 8) return;
            column = kSquare[slot][0];
            row = kSquare[slot][1];
        }

    }

    PixelImage scaleNearest(const PixelImage& image, int scale) {
        if (scale <= 1) return image;
        PixelImage out(image.width() * scale, image.height() * scale);
        std::span<uint8_t> dst = out.editPixels();
        std::span<const uint8_t> src = image.pixels();
        for (int y = 0; y < out.height(); y++) {
            for (int x = 0; x < out.width(); x++) {
                const size_t from = (size_t)(y / scale * image.width() + x / scale) * 4;
                const size_t to = (size_t)(y * out.width() + x) * 4;
                for (size_t c = 0; c < 4; c++) dst[to + c] = src[from + c];
            }
        }
        return out;
    }

    TilesetPagePixels::TilesetPagePixels():
        loader([](const std::string& path, std::string* error) -> std::optional<PixelImage> {
            std::string file = path;
            if (file.find('#') != std::string::npos && !ImportedAssets::find(ImportedAssets::normalize(file), file)) {
                if (error) *error = "no imported file for " + path;
                return std::nullopt;
            }
            return sprite::loadSpriteImage(FileData::getSystemPath(file), error);
        }) {
    }

    TilesetPagePixels::TilesetPagePixels(Loader loader): loader(std::move(loader)) {
    }

    const PixelImage* TilesetPagePixels::get(const std::string& path) {
        auto it = pages.find(path);
        if (it == pages.end()) {
            std::string why;
            it = pages.emplace(path, loader(path, &why)).first;
            if (!it->second) error = why;
        }
        return it->second ? &*it->second : nullptr;
    }

    bool createFromTexture(const std::string& name, const AssetRef& texture, const TextureSlicing& slicing, const PixelImage& image,
                           TilesetData& out, PixelProblem& error) {
        SliceGrid grid;
        if (!sliceGridOf(slicing, image, grid, error)) return false;
        out = TilesetData();
        out.name = name;
        out.tileWidth = slicing.tileWidth;
        out.tileHeight = slicing.tileHeight;
        const TilesetSource& source = addTextureSourceEntry(out, texture, slicing);
        sliceAddTiles(out, source.id, slicing, image, grid);
        return true;
    }

    bool addTextureSource(TilesetData& data, const AssetRef& texture, const TextureSlicing& slicing, const PixelImage& image, int& sourceId,
                          PixelProblem& error) {
        SliceGrid grid;
        if (!sliceGridOf(slicing, image, grid, error)) return false;
        const TilesetSource& source = addTextureSourceEntry(data, texture, slicing);
        sourceId = source.id;
        sliceAddTiles(data, sourceId, slicing, image, grid);
        return true;
    }

    bool resliceTexture(TilesetData& data, int sourceId, const TextureSlicing& slicing, const PixelImage& image, ResliceReport& report,
                        PixelProblem& error) {
        report = ResliceReport();
        TilesetSource* source = nullptr;
        for (TilesetSource& candidate : data.sources) {
            if (candidate.id == sourceId) source = &candidate;
        }
        if (!source || source->kind != TilesetSourceKind::Texture) {
            error = tilesetEditProblem("unknown_tile", "the tileset has no texture source " + std::to_string(sourceId), "re-slice a texture source", "source");
            return false;
        }
        SliceGrid grid;
        if (!sliceGridOf(slicing, image, grid, error)) return false;
        const int oldWidth = source->tileWidth > 0 ? source->tileWidth : data.tileWidth;
        const int oldHeight = source->tileHeight > 0 ? source->tileHeight : data.tileHeight;
        const int oldMargin = source->margin, oldSpacing = source->spacing;

        // a tile is identified by where its pixels start, the only thing a new grid and the old
        // one can agree on when the slicing itself changed
        std::map<std::pair<int, int>, int32_t> byOrigin;
        for (const TileDef& tile : data.tiles) {
            if (tile.source != sourceId) continue;
            byOrigin[{oldMargin + tile.atX * (oldWidth + oldSpacing), oldMargin + tile.atY * (oldHeight + oldSpacing)}] = tile.id;
        }

        const int stepX = slicing.tileWidth + slicing.spacing, stepY = slicing.tileHeight + slicing.spacing;
        for (int row = 0; row < grid.rows; row++) {
            for (int column = 0; column < grid.columns; column++) {
                const int x = slicing.margin + column * stepX, y = slicing.margin + row * stepY;
                const auto found = byOrigin.find({x, y});
                if (found != byOrigin.end()) {
                    TileDef* tile = data.findTile(found->second);
                    tile->atX = column;
                    tile->atY = row;
                    report.kept.push_back(found->second);
                    byOrigin.erase(found);
                } else if (!slicing.skipEmpty || !sliceIsEmpty(image, x, y, slicing.tileWidth, slicing.tileHeight)) {
                    report.added.push_back(data.addTile(sourceId, column, row).id);
                }
            }
        }
        for (const auto& entry : byOrigin) {
            report.removed.push_back(entry.second);
            data.removeTile(entry.second);
        }
        std::sort(report.kept.begin(), report.kept.end());
        std::sort(report.removed.begin(), report.removed.end());
        source->tileWidth = slicing.tileWidth;
        source->tileHeight = slicing.tileHeight;
        source->margin = slicing.margin;
        source->spacing = slicing.spacing;
        source->skipEmpty = slicing.skipEmpty;
        return true;
    }

    bool addSheetSource(TilesetData& data, const AssetRef& sheetRef, const std::vector<std::string>& frames, int& sourceId, PixelProblem& error) {
        const std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(sheetRef.path);
        if (!sheet) {
            error = tilesetEditProblem("tileset_missing", "cannot load the sheet " + sheetRef.path, "use a sheet SpriteSheetPool can load (.aseprite, .spriteatlas, texture with a Sprite Mode)", "sheet");
            return false;
        }
        return addSheetSource(data, sheetRef, *sheet, frames, sourceId, error);
    }

    bool addSheetSource(TilesetData& data, const AssetRef& sheetRef, const SpriteSheetData& sheet, const std::vector<std::string>& frames, int& sourceId,
                        PixelProblem& error) {
        std::vector<std::string> names = frames;
        if (names.empty()) {
            for (const SpriteFrame& frame : sheet.frames) names.push_back(frame.name);
        }
        for (size_t i = 0; i < names.size(); i++) {
            if (sheet.findFrame(names[i]) < 0) {
                error = tilesetEditProblem("unknown_tile", "the sheet has no frame \"" + names[i] + "\"", "use a frame name of the sheet", "frames[" + std::to_string(i) + "]");
                return false;
            }
        }
        TilesetSource source;
        source.id = nextSourceId(data);
        source.kind = TilesetSourceKind::Sheet;
        source.asset = sheetRef;
        source.frames = frames;
        sourceId = source.id;
        data.sources.push_back(std::move(source));
        for (const std::string& name : names) {
            TileDef& tile = data.addTile(sourceId, 0, 0);
            tile.frame = name;
        }
        return true;
    }

    bool addAsepriteSource(TilesetData& data, const AssetRef& file, const pixelart::aseprite::TilesetOutput& tileset, int tilesetIndex,
                           std::vector<std::string>& warnings, int& sourceId, PixelProblem& error) {
        if (tileset.tileWidth <= 0 || tileset.tileHeight <= 0 || tileset.tileCount < 1) {
            error = tilesetEditProblem("tileset_missing", "the Aseprite tileset '" + tileset.name + "' has no usable tiles", "give it tiles in Aseprite", "tileset");
            return false;
        }
        TilesetSource source;
        source.id = nextSourceId(data);
        source.kind = TilesetSourceKind::Aseprite;
        source.asset = file;
        source.asepriteTileset = tilesetIndex;
        sourceId = source.id;
        data.sources.push_back(std::move(source));
        for (int i = 1; i < tileset.tileCount; i++) {
            TileDef& tile = data.addTile(sourceId, i - 1, 0);
            if ((size_t)i < tileset.tileUserData.size()) readAsepriteUserData(data, tile, tileset.tileUserData[(size_t)i], warnings);
        }
        return true;
    }

    bool addAsepriteSourceFromFile(TilesetData& data, const AssetRef& file, const std::filesystem::path& absolute, int tilesetIndex,
                                   std::vector<std::string>& warnings, int& sourceId, PixelProblem& error) {
        namespace ase = pixelart::aseprite;
        ase::ReadOptions options;
        options.provenance = false;
        ase::ReadResult read = ase::readFile(absolute, options);
        if (!read.ok()) {
            error = read.error;
            return false;
        }
        ase::AsepriteImportContext context;
        context.sourcePath = file.path;
        const ase::AsepriteImport import = ase::AsepriteImport::run(*read.document, ase::AsepriteImportSettings(), context);
        if (!import.ok()) {
            error = import.error;
            return false;
        }
        if (tilesetIndex < 0 || (size_t)tilesetIndex >= import.tilesets().size()) {
            error = tilesetEditProblem("tileset_missing", "the file has no tileset " + std::to_string(tilesetIndex), "use a tileset index of the file", "tileset");
            return false;
        }
        return addAsepriteSource(data, file, import.tilesets()[(size_t)tilesetIndex], tilesetIndex, warnings, sourceId, error);
    }

    bool tileImage(const TilesetRuntime& tileset, int32_t id, TilesetPagePixels& pages, PixelImage& out) {
        const TileRenderInfo* info = tileset.tile(id);
        if (!info || info->width <= 0 || info->height <= 0 || info->page >= tileset.pages.size()) return false;
        const PixelImage* page = pages.get(tileset.pages[info->page]);
        if (!page) return false;
        const double w = page->width(), h = page->height();
        const pixelart::IntRect rect{(int)std::lround(info->u0 * w), (int)std::lround(info->v0 * h),
                                     (int)std::lround((info->u1 - info->u0) * w), (int)std::lround((info->v1 - info->v0) * h)};
        out = PixelImage(info->width, info->height);
        out.blit(page->crop(rect), (int)info->x0, info->height - (int)info->y1);
        return true;
    }

    bool fillAlphaCollisions(TilesetData& data, const TilesetRuntime& tileset, TilesetPagePixels& pages, const std::vector<int32_t>& tiles,
                             size_t& filled, PixelProblem& error) {
        filled = 0;
        std::vector<int32_t> ids = tiles;
        if (ids.empty()) {
            for (const TileDef& tile : data.tiles) {
                if (tile.collision.mode == TileCollisionMode::Alpha) ids.push_back(tile.id);
            }
        }
        // the outlines are gathered first so a failure leaves the document as it was
        std::vector<std::pair<int32_t, std::vector<TilePolygon>>> traced;
        traced.reserve(ids.size());
        for (const int32_t id : ids) {
            const TileDef* tile = data.findTile(id);
            if (!tile) {
                error = tilesetEditProblem("unknown_tile", "the tileset has no tile " + std::to_string(id), "use a tile id the tileset has", "tiles");
                return false;
            }
            PixelImage image;
            if (!tileImage(tileset, id, pages, image)) {
                error = tilesetEditProblem("tileset_missing", "tile " + std::to_string(id) + " has no pixels: " + pages.lastError(), "check its source's texture", "tiles");
                return false;
            }
            const std::span<const uint8_t> rgba = image.pixels();
            traced.emplace_back(id, traceAlpha(rgba.data(), image.width(), image.height(), 0, 0, image.width(), image.height(), tile->collision.threshold));
        }
        for (auto& entry : traced) {
            data.findTile(entry.first)->collision.polygons = std::move(entry.second);
            filled++;
        }
        return true;
    }

    PixelImage contactSheet(const TilesetRuntime& tileset, TilesetPagePixels& pages, const ContactSheetOptions& options) {
        if (!tileset.data || tileset.data->tiles.empty()) return PixelImage();
        const int scale = std::max(options.scale, 1);
        const int columns = std::max(options.columns, 1);
        int tileW = 1, tileH = 1;
        for (const TileDef& tile : tileset.data->tiles) {
            if (const TileRenderInfo* info = tileset.tile(tile.id)) {
                tileW = std::max(tileW, info->width);
                tileH = std::max(tileH, info->height);
            }
        }
        constexpr int kLabelHeight = 7;   // 5 glyph rows, 1 above and 1 below
        constexpr int kGap = 1;
        const int boxW = tileW * scale, boxH = tileH * scale + kLabelHeight;
        const int count = (int)tileset.data->tiles.size();
        const int rows = (count + columns - 1) / columns;
        PixelImage sheet(columns * (boxW + kGap) + kGap, rows * (boxH + kGap) + kGap);
        sheet.fill({32, 32, 40, 255});
        for (int index = 0; index < count; index++) {
            const TileDef& tile = tileset.data->tiles[(size_t)index];
            const int x = kGap + index % columns * (boxW + kGap);
            const int y = kGap + index / columns * (boxH + kGap);
            PixelImage image;
            if (tileImage(tileset, tile.id, pages, image)) sheet.blendOver(scaleNearest(image, scale), x, y + (tileH * scale - image.height() * scale), 255);
            if (options.drawIds) contactDrawDigits(sheet, x, y + tileH * scale + 1, std::to_string(tile.id), {255, 255, 255, 255});
        }
        return sheet;
    }

    std::vector<PixelImage> coverageImages(const TilesetData& data, int set, const TerrainCoverage& coverage, int cellPixels) {
        std::vector<PixelImage> images;
        images.reserve(coverage.missing.size());
        if (set < 0 || (size_t)set >= data.terrainSets.size()) return images;
        const int unit = std::max(cellPixels, 1);
        const TerrainSetDef& terrainSet = data.terrainSets[(size_t)set];
        const pixelart::PixelRgba noneColor{24, 24, 28, 255};
        const auto fillCell = [&](PixelImage& image, int column, int row, const auto& color) {
            for (int y = 0; y < unit; y++) {
                for (int x = 0; x < unit; x++) image.set(column * unit + x, row * unit + y, color(x, y));
            }
        };
        const auto terrainColor = [&](int local) -> pixelart::PixelRgba {
            return local >= 0 && (size_t)local < terrainSet.terrains.size() ? coverageColor(terrainSet.terrains[(size_t)local].color) : noneColor;
        };
        for (const TerrainMissingMask& mask : coverage.missing) {
            PixelImage image(3 * unit, 3 * unit);
            for (int row = 0; row < 3; row++) {
                for (int column = 0; column < 3; column++) fillCell(image, column, row, [&](int, int) { return noneColor; });
            }
            const pixelart::PixelRgba centre = terrainColor(mask.center);
            fillCell(image, 1, 1, [&](int, int) { return centre; });
            for (int slot = 0; slot < MAX_PEERING_SLOTS; slot++) {
                int column = 0, row = 0;
                coverageCellOfSlot(data.shape, slot, column, row);
                if (column < 0) continue;
                const int16_t value = mask.peering[(size_t)slot];
                if (value == PEERING_ANY) {
                    fillCell(image, column, row, [](int x, int y) { return (x + y) % 2 == 0 ? pixelart::PixelRgba{90, 90, 96, 255} : pixelart::PixelRgba{60, 60, 66, 255}; });
                } else if (value != PEERING_NONE) {
                    const pixelart::PixelRgba color = terrainColor(value);
                    fillCell(image, column, row, [&](int, int) { return color; });
                }
            }
            images.push_back(std::move(image));
        }
        return images;
    }

    std::string tilesetPathForTexture(const std::string& texturePath) {
        std::filesystem::path path(texturePath);
        path.replace_extension(".tileset");
        return path.generic_string();
    }

    WriteFilesCmd::Item tilesetFileItem(const std::filesystem::path& absolute, const TilesetData& data) {
        return WriteFilesCmd::Item{absolute, tileset::write(data)};
    }

    bool writeTilesetFile(Project* project, const std::filesystem::path& absolute, const TilesetData& data, std::string& error, const std::string& stepName) {
        const auto afterChange = [project](const std::vector<std::filesystem::path>& changed) {
            ProjectFileWrites::filesWritten(project, changed);
            for (const std::filesystem::path& path : changed) TilesetPool::invalidate(path.generic_string());
        };
        return ProjectFileWrites::writeUndoable(project, {tilesetFileItem(absolute, data)}, error, afterChange, nullptr, {}, false, stepName);
    }

}
