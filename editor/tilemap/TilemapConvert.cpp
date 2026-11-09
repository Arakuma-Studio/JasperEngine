// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapConvert.h"

#include "Catalog.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "component/GridComponent.h"
#include "component/ScriptComponent.h"
#include "component/TilemapLayerComponent.h"
#include "io/FileData.h"
#include "sprite/SpriteSheetSources.h"
#include "tilemap/TilemapCellsCmd.h"
#include "tilemap/TilesetEdit.h"
#include "tilemap/TilesetPool.h"
#include "util/ProjectFileWrites.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace jasperengine::editor::tilemap {

    namespace {

        constexpr float kConvertEpsilon = 0.001f;
        constexpr size_t kMaxOffenders = 10;
        constexpr int kMaxSliceSpacing = 256;

        bool convertNearInt(float value, int& out) {
            out = (int)std::lround(value);
            return std::fabs(value - (float)out) < kConvertEpsilon;
        }

        std::string convertRectTexture(const TileRectData& rect, const MeshComponent& mesh) {
            if (mesh.numSubmeshes == 0) return {};
            const unsigned int submesh = rect.submeshId >= 0 && (unsigned int)rect.submeshId < mesh.numSubmeshes ? (unsigned int)rect.submeshId : 0u;
            const std::string own = mesh.submeshes[submesh].material.baseColorTexture.getPath();
            return own.empty() ? mesh.submeshes[0].material.baseColorTexture.getPath() : own;
        }

        struct ConvertRectPixels {
            bool usable = false;
            std::string reason;
            std::string texture;
            int x = 0, y = 0, w = 0, h = 0;
        };

        ConvertRectPixels convertRectPixels(const TileRectData& rect, const MeshComponent& mesh) {
            ConvertRectPixels out;
            out.texture = convertRectTexture(rect, mesh);
            if (out.texture.empty()) {
                out.reason = "its rect has no texture";
                return out;
            }
            float x = rect.rect.getX(), y = rect.rect.getY(), w = rect.rect.getWidth(), h = rect.rect.getHeight();
            if (rect.rect.isNormalized()) {
                int textureW = 0, textureH = 0;
                if (!SpriteSheetSources::textureSize(out.texture, textureW, textureH) || textureW <= 0 || textureH <= 0) {
                    out.reason = "the size of its texture is unknown";
                    return out;
                }
                x *= (float)textureW;
                y *= (float)textureH;
                w *= (float)textureW;
                h *= (float)textureH;
            }
            if (!convertNearInt(x, out.x) || !convertNearInt(y, out.y) || !convertNearInt(w, out.w) || !convertNearInt(h, out.h)) {
                out.reason = "its rect is not on whole pixels";
                return out;
            }
            if (out.w <= 0 || out.h <= 0) {
                out.reason = "its rect is empty";
                return out;
            }
            if (out.x < 0 || out.y < 0) {
                out.reason = "its rect starts outside the texture";
                return out;
            }
            out.usable = true;
            return out;
        }

        // the one margin and spacing that place every rect at margin + at * (size + spacing), on
        // both axes (Tiled's texture slicing); legacy rects are free, a tileset's are a grid
        bool convertFitSlice(const std::vector<const ConvertRectPixels*>& rects, int width, int height, int& margin, int& spacing) {
            int gapX = 0, gapY = 0, minX = rects.front()->x, minY = rects.front()->y;
            for (const ConvertRectPixels* rect : rects) {
                gapX = std::gcd(gapX, std::abs(rect->x - rects.front()->x));
                gapY = std::gcd(gapY, std::abs(rect->y - rects.front()->y));
                minX = std::min(minX, rect->x);
                minY = std::min(minY, rect->y);
            }
            for (int s = 0; s <= kMaxSliceSpacing; s++) {
                const int stepX = width + s, stepY = height + s;
                if ((gapX != 0 && gapX % stepX != 0) || (gapY != 0 && gapY % stepY != 0)) continue;
                const int residueX = minX % stepX, residueY = minY % stepY;
                for (int m = 0; m <= std::min(minX, minY); m++) {
                    if (m % stepX == residueX && m % stepY == residueY) {
                        margin = m;
                        spacing = s;
                        return true;
                    }
                }
            }
            return false;
        }

        bool convertRectFits(const ConvertRectPixels& rect, int width, int height, int margin, int spacing) {
            if (rect.w != width || rect.h != height || rect.x < margin || rect.y < margin) return false;
            return (rect.x - margin) % (width + spacing) == 0 && (rect.y - margin) % (height + spacing) == 0;
        }

        // a standalone "Tilemap" token (or TilemapComponent), which a script's text uses to reach
        // the legacy component; TilemapLayer and the like do not count
        bool convertScriptUsesTilemap(const std::string& text) {
            const auto isWord = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
            static const std::string kWord = "Tilemap";
            size_t at = text.find(kWord);
            while (at != std::string::npos) {
                const size_t end = at + kWord.size();
                const bool leftOk = at == 0 || !isWord(text[at - 1]);
                const bool component = text.compare(end, 9, "Component") == 0;
                const size_t after = component ? end + 9 : end;
                const bool rightOk = after >= text.size() || !isWord(text[after]);
                if (leftOk && rightOk) return true;
                at = text.find(kWord, at + 1);
            }
            return false;
        }

        // The Mesh the legacy build filled: the layer's system starts from an empty buffer and one
        // submesh (TilemapSystem::prepareTileLayerMesh), and the legacy build rebuilds it after an
        // undo, so only what neither of them restores is kept here.
        class ConvertMeshCmd : public Command {
        public:
            ConvertMeshCmd(Project* project, uint32_t sceneId, Entity entity): project(project), sceneId(sceneId), entity(entity) {}

            bool execute() override {
                MeshComponent* mesh = meshNow();
                if (!mesh) return false;
                oldNumSubmeshes = mesh->numSubmeshes;
                oldTileLayer = mesh->tileLayer;
                oldTexture = mesh->numSubmeshes > 0 ? mesh->submeshes[0].material.baseColorTexture.getPath() : std::string();
                mesh->buffer.clear();
                mesh->indices.clear();
                mesh->numSubmeshes = 1;
                if (mesh->loaded) mesh->needReload = true;
                touchScene();
                return true;
            }

            void undo() override {
                MeshComponent* mesh = meshNow();
                if (!mesh) return;
                mesh->numSubmeshes = oldNumSubmeshes;
                mesh->tileLayer = oldTileLayer;
                if (oldNumSubmeshes > 0 && mesh->submeshes[0].material.baseColorTexture.getPath() != oldTexture) {
                    mesh->submeshes[0].material.baseColorTexture.setPath(oldTexture);
                    mesh->submeshes[0].needUpdateTexture = true;
                }
                if (mesh->loaded) mesh->needReload = true;
                touchScene();
            }

            bool affectsStructure() const override { return false; }
            std::string describe() const override { return "Convert Tilemap Mesh"; }
            const char* typeName() const override { return "ConvertMeshCmd"; }

        private:
            Project* project;
            uint32_t sceneId;
            Entity entity;
            unsigned int oldNumSubmeshes = 0;
            bool oldTileLayer = false;
            std::string oldTexture;

            MeshComponent* meshNow() const {
                SceneProject* sceneProject = project->getScene(sceneId);
                return sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<MeshComponent>(entity) : nullptr;
            }
            void touchScene() const {
                if (SceneProject* sceneProject = project->getScene(sceneId)) sceneProject->needUpdateRender = true;
            }
        };

    }

    ConvertCheck check(const TilemapComponent& tilemap, const MeshComponent& mesh) {
        ConvertCheck result;
        const auto addOffender = [&](int index, const TileData& tile, std::string reason) {
            result.offenderCount++;
            if (result.offenders.size() < kMaxOffenders) {
                ConvertOffender offender;
                offender.tile = index;
                offender.name = tile.name;
                offender.x = tile.position.x;
                offender.y = tile.position.y;
                offender.width = tile.width;
                offender.height = tile.height;
                offender.reason = std::move(reason);
                result.offenders.push_back(std::move(offender));
            }
        };

        // ---- the rects as pixels, and the tile size from the first tile that has one
        std::vector<ConvertRectPixels> pixels(tilemap.numTilesRect);
        for (unsigned int i = 0; i < tilemap.numTilesRect; i++) pixels[i] = convertRectPixels(tilemap.tilesRect[i], mesh);
        int tileW = 0, tileH = 0;
        bool haveSize = false;
        for (unsigned int i = 0; i < tilemap.numTiles && !haveSize; i++) {
            const TileData& tile = tilemap.tiles[i];
            if (tile.width == 0.0f && tile.height == 0.0f) continue;
            haveSize = convertNearInt(tile.width, tileW) && convertNearInt(tile.height, tileH) && tileW > 0 && tileH > 0;
        }
        if (!haveSize) {
            for (const ConvertRectPixels& rect : pixels) {
                if (rect.usable) {
                    tileW = rect.w;
                    tileH = rect.h;
                    haveSize = true;
                    break;
                }
            }
        }
        if (!haveSize) {
            result.problem.kind = "tileset_missing";
            result.problem.message = "the map has no tile rects with pixels";
            result.problem.fix = "add tile rects with a texture, or remove the Tilemap";
            return result;
        }
        result.tileWidth = tileW;
        result.tileHeight = tileH;

        // ---- which rects the tiles use, and each texture's slicing from them
        std::set<int> usedRects;
        for (unsigned int i = 0; i < tilemap.numTiles; i++) {
            const TileData& tile = tilemap.tiles[i];
            if (tile.width == 0.0f && tile.height == 0.0f) continue;
            if (tile.rectId >= 0 && (unsigned int)tile.rectId < tilemap.numTilesRect) usedRects.insert(tile.rectId);
        }
        std::vector<std::string> textureOrder;
        std::map<std::string, std::vector<int>> rectsOfTexture;
        for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
            if (pixels[i].texture.empty()) continue;
            if (rectsOfTexture.find(pixels[i].texture) == rectsOfTexture.end()) textureOrder.push_back(pixels[i].texture);
            rectsOfTexture[pixels[i].texture].push_back((int)i);
        }

        result.rects.assign(tilemap.numTilesRect, ConvertRect());
        std::vector<std::string> rectReason(tilemap.numTilesRect);
        for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
            if (!pixels[i].usable) rectReason[i] = pixels[i].reason;
            else if (pixels[i].w != tileW || pixels[i].h != tileH) rectReason[i] = "its rect is not the tile size";
        }

        for (const std::string& texture : textureOrder) {
            const std::vector<int>& indices = rectsOfTexture[texture];
            // the slice comes from the rects tiles use; rects no tile uses only join if they fit it
            std::vector<const ConvertRectPixels*> forFit;
            for (int index : indices) {
                if (rectReason[(size_t)index].empty() && usedRects.count(index) != 0) forFit.push_back(&pixels[(size_t)index]);
            }
            if (forFit.empty()) {
                for (int index : indices) {
                    if (rectReason[(size_t)index].empty()) forFit.push_back(&pixels[(size_t)index]);
                }
            }
            ConvertSource source;
            source.texture = texture;
            if (forFit.empty() || !convertFitSlice(forFit, tileW, tileH, source.margin, source.spacing)) {
                for (int index : indices) {
                    if (rectReason[(size_t)index].empty()) rectReason[(size_t)index] = "its rect is off the texture's tile grid";
                }
                continue;
            }
            const int sourceIndex = (int)result.sources.size();
            result.sources.push_back(source);
            for (int index : indices) {
                if (!rectReason[(size_t)index].empty()) continue;
                const ConvertRectPixels& rect = pixels[(size_t)index];
                if (!convertRectFits(rect, tileW, tileH, source.margin, source.spacing)) {
                    rectReason[(size_t)index] = "its rect is off the texture's tile grid";
                    continue;
                }
                ConvertRect& out = result.rects[(size_t)index];
                out.valid = true;
                out.source = sourceIndex;
                out.atX = (rect.x - source.margin) / (tileW + source.spacing);
                out.atY = (rect.y - source.margin) / (tileH + source.spacing);
            }
        }

        // ---- the tiles
        std::unordered_map<CellCoord, int, CellCoordHash> occupied;
        for (unsigned int i = 0; i < tilemap.numTiles; i++) {
            const TileData& tile = tilemap.tiles[i];
            if (tile.width == 0.0f && tile.height == 0.0f) continue;
            int w = 0, h = 0;
            if (!convertNearInt(tile.width, w) || !convertNearInt(tile.height, h) || w != tileW || h != tileH) {
                addOffender((int)i, tile, "its size is not the map's tile size");
                continue;
            }
            int px = 0, py = 0;
            if (!convertNearInt(tile.position.x, px) || !convertNearInt(tile.position.y, py) || px < 0 || py < 0 ||
                px % tileW != 0 || py % tileH != 0) {
                addOffender((int)i, tile, "its position is not on the tile grid");
                continue;
            }
            if (tile.rectId < 0 || (unsigned int)tile.rectId >= tilemap.numTilesRect) {
                addOffender((int)i, tile, "it names a tile rect that does not exist");
                continue;
            }
            if (!rectReason[(size_t)tile.rectId].empty()) {
                addOffender((int)i, tile, rectReason[(size_t)tile.rectId]);
                continue;
            }
            const CellCoord cell{px / tileW, py / tileH};
            const auto inserted = occupied.emplace(cell, (int)i);
            if (!inserted.second) {
                addOffender((int)i, tile, "another tile (" + std::to_string(inserted.first->second) + ") is in the same cell");
                continue;
            }
            result.cells.push_back(ConvertCell{cell, tile.rectId});
        }

        if (result.offenderCount > 0) {
            result.problem.kind = "not_grid_aligned";
            result.problem.message = std::to_string(result.offenderCount) + " tiles do not fit one grid of " + std::to_string(tileW) + " x " + std::to_string(tileH);
            result.problem.fix = "give every tile the same size and a position on that size's grid";
            return result;
        }

        for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
            if (!result.rects[i].valid) {
                result.warnings.push_back("tile rect " + std::to_string(i) + " ('" + tilemap.tilesRect[i].name + "') gets no tile: " + rectReason[i]);
            }
        }
        if (tilemap.flipY && !tilemap.automaticFlipY) result.warnings.push_back("Flip Y is dropped: tiles draw upright");
        if (tilemap.textureScaleFactor != 0.0f) result.warnings.push_back("Texture Scale Factor is dropped: tiles are clamped inside their frame");
        result.ok = true;
        return result;
    }

    ConvertResult convert(Project* project, uint32_t sceneId, Entity entity) {
        ConvertResult result;
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene) {
            result.problem.kind = "unknown_layer";
            result.problem.message = "the scene is not open";
            result.problem.fix = "open the scene first";
            return result;
        }
        Scene* scene = sceneProject->scene;
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!tilemap || !mesh) {
            result.problem.kind = "unknown_layer";
            result.problem.message = "the GameObject has no Free Tilemap";
            result.problem.fix = "select a GameObject with a Free Tilemap (Legacy) component";
            return result;
        }
        result.check = check(*tilemap, *mesh);
        if (!result.check.ok) {
            result.problem = result.check.problem;
            return result;
        }
        if (scene->findComponent<GridComponent>(entity) || scene->findComponent<TilemapLayerComponent>(entity)) {
            result.problem.kind = "layout_mismatch";
            result.problem.message = "the GameObject already has a Grid or Tilemap Layer";
            result.problem.fix = "remove it, then convert";
            return result;
        }
        result.warnings = result.check.warnings;

        if (tilemap->pixelsPerUnit > 0.0f && std::lround(tilemap->pixelsPerUnit) != Engine::getPixelsPerUnit()) {
            result.warnings.push_back("Pixels Per Unit " + std::to_string(tilemap->pixelsPerUnit) + " is dropped: layers use the project's");
        }

        // ---- scripts on the GameObject that use the legacy API
        if (const ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)) {
            for (const ScriptEntry& entry : scripts->scripts) {
                if (entry.path.path.empty()) continue;
                std::ifstream file(FileData::getSystemPath(entry.path.path), std::ios::binary);
                if (!file) continue;
                std::stringstream text;
                text << file.rdbuf();
                if (convertScriptUsesTilemap(text.str())) result.scriptsUsingTilemap.push_back(entry.path.path);
            }
        }

        // ---- the tileset
        const std::string firstTexture = result.check.sources.empty() ? std::string() : result.check.sources.front().texture;
        TilesetData data;
        data.name = fs::path(firstTexture).stem().generic_string();
        data.tileWidth = result.check.tileWidth;
        data.tileHeight = result.check.tileHeight;
        for (size_t i = 0; i < result.check.sources.size(); i++) {
            TilesetSource source;
            source.id = (int)i;
            source.kind = TilesetSourceKind::Texture;
            source.asset = AssetRef(result.check.sources[i].texture);
            source.tileWidth = result.check.tileWidth;
            source.tileHeight = result.check.tileHeight;
            source.margin = result.check.sources[i].margin;
            source.spacing = result.check.sources[i].spacing;
            source.skipEmpty = false;    // the map's rects decide what exists, not the pixels
            data.sources.push_back(std::move(source));
        }
        for (unsigned int i = 0; i < tilemap->numTilesRect; i++) {
            const ConvertRect& rect = result.check.rects[i];
            if (!rect.valid) continue;
            TileDef tile;
            tile.id = (int32_t)i;
            tile.source = rect.source;
            tile.atX = rect.atX;
            tile.atY = rect.atY;
            tile.name = tilemap->tilesRect[i].name;
            data.tiles.push_back(std::move(tile));
        }
        data.nextId = (int32_t)tilemap->numTilesRect;
        std::vector<TilesetProblem> problems;
        tileset::validate(data, problems);
        for (const TilesetProblem& problem : problems) result.warnings.push_back(problem.field + ": " + problem.message);

        // ---- the file next to the texture, not over someone else's tileset
        const fs::path assetsRoot = project->getAssetsPath();
        const fs::path texturePath(firstTexture);
        const fs::path absoluteBase = (texturePath.is_absolute() ? texturePath : assetsRoot / texturePath).lexically_normal();
        fs::path absoluteTileset = absoluteBase;
        absoluteTileset.replace_extension(".tileset");
        fs::path assetTileset = texturePath;
        assetTileset.replace_extension(".tileset");
        for (int suffix = 2; fs::exists(absoluteTileset); suffix++) {
            const std::string name = absoluteBase.stem().generic_string() + "_" + std::to_string(suffix) + ".tileset";
            absoluteTileset = absoluteBase.parent_path() / name;
            assetTileset = texturePath.parent_path() / name;
        }
        result.tilesetPath = assetTileset.generic_string();

        // ---- one undo step
        const std::vector<ConvertCell> plannedCells = result.check.cells;
        std::vector<TileCellChange> changes;
        changes.reserve(plannedCells.size());
        for (const ConvertCell& cell : plannedCells) {
            TileCellChange change;
            change.cell = cell.cell;
            change.newValue = makeCellValue(cell.tile, 0);
            changes.push_back(change);
        }
        const float ppu = (float)Engine::getPixelsPerUnit();
        const Vector2 cellSize((float)result.check.tileWidth / ppu, (float)result.check.tileHeight / ppu);
        const Vector4 tint = mesh->numSubmeshes > 0 ? mesh->submeshes[0].material.baseColorFactor : Vector4(1.0f, 1.0f, 1.0f, 1.0f);

        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t mark = history->mark();
        const auto rollback = [&]() {
            while (history->position() > mark && history->undo()) {}
        };
        const auto add = [&](Command* command, const char* what) {
            if (history->addCommandNoMerge(command)) return true;
            result.problem.kind = "tileset_missing";
            result.problem.message = std::string("the conversion could not ") + what;
            result.problem.fix = "check the project's files, then try again";
            rollback();
            return false;
        };

        auto writeCommand = std::make_unique<WriteFilesCmd>(
            std::vector<WriteFilesCmd::Item>{tilesetFileItem(absoluteTileset, data)},
            [project](const std::vector<fs::path>& changed) {
                ProjectFileWrites::filesWritten(project, changed);
                for (const fs::path& path : changed) TilesetPool::invalidate(path.generic_string());
            });
        writeCommand->setName("Create Tileset " + absoluteTileset.filename().generic_string());
        if (!add(writeCommand.release(), "write the tileset file")) return result;
        if (!add(new AddComponentCmd(project, sceneId, entity, ComponentType::GridComponent), "add the Grid")) return result;
        if (!add(new AddComponentCmd(project, sceneId, entity, ComponentType::TilemapLayerComponent), "add the Tilemap Layer")) return result;
        auto properties = std::make_unique<MultiPropertyCmd>();
        properties->addPropertyCmd<Vector2>(project, sceneId, entity, ComponentType::GridComponent, "cellSize", cellSize);
        properties->addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::TilemapLayerComponent, "tileset", result.tilesetPath);
        properties->addPropertyCmd<Vector4>(project, sceneId, entity, ComponentType::TilemapLayerComponent, "tint", tint);
        if (!add(properties.release(), "set the layer's fields")) return result;
        if (!add(new TilemapCellsCmd(project, sceneId, entity, std::move(changes), "Convert Tilemap Cells"), "write the cells")) return result;
        if (!add(new RemoveComponentCmd(project, sceneId, entity, ComponentType::TilemapComponent), "remove the legacy Tilemap")) return result;
        if (!add(new ConvertMeshCmd(project, sceneId, entity), "reset the Mesh")) return result;
        history->groupSince(mark, "Convert to Grid Tilemap");
        result.ok = true;
        return result;
    }

}
