// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/import/TilemapImportApply.h"

#include "Project.h"
#include "Stream.h"
#include "Out.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "component/GridComponent.h"
#include "tilemap/TilemapMeshAccess.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "Engine.h"
#include "tilemap/TerrainSolver.h"
#include "tilemap/TileCells.h"
#include "tilemap/TilemapCellsCmd.h"
#include "tilemap/Tileset.h"
#include "tilemap/TilesetEdit.h"
#include "tilemap/TilesetPool.h"
#include "tilemap/import/LdtkImporter.h"
#include "tilemap/import/TiledImporter.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace jasperengine::editor::tilemapimport {

    namespace {

        ImportResult applyFailure(ImportResult result, const std::string& message, const std::string& fix) {
            result.ok = false;
            result.problem.kind = "import_failed";
            result.problem.message = message;
            result.problem.fix = fix;
            return result;
        }

        bool readTextFile(const fs::path& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        std::string lowerExtension(const fs::path& path) {
            std::string ext = path.extension().generic_string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        std::string sanitizedFileName(const std::string& name) {
            std::string out;
            for (char c : name) {
                const auto u = static_cast<unsigned char>(c);
                out.push_back(std::isalnum(u) || c == '-' || c == '_' || c == '.' ? c : '_');
            }
            return out.empty() ? std::string("tileset") : out;
        }

        // where a plan tileset's file goes, and its source images as assets-relative paths
        struct TilesetFile {
            fs::path absolute;
            std::string assetPath;
        };

        bool isBeneath(const fs::path& path, const fs::path& root) {
            const std::string relative = path.lexically_normal().lexically_relative(root.lexically_normal()).generic_string();
            return !relative.empty() && relative.rfind("..", 0) != 0;
        }

        // A file for each tileset, `kept` holding the ones a linked Grid already has (its layers'
        // files, overwritten); a new one never lands on another file: the first free name wins
        std::vector<TilesetFile> tilesetFiles(const ImportPlan& plan, const ImportTarget& target, const std::map<int, std::string>& kept) {
            Project* project = target.project;
            const fs::path assets = project->getAssetsPath();
            const fs::path dir = target.tilesetsDir.empty() ? target.mapFile.parent_path() : target.tilesetsDir;
            std::vector<TilesetFile> out(plan.tilesets.size());
            std::set<std::string> taken;
            for (size_t i = 0; i < plan.tilesets.size(); i++) {
                const auto own = kept.find(static_cast<int>(i));
                if (own != kept.end()) {
                    out[i].assetPath = own->second;
                    out[i].absolute = (assets / own->second).lexically_normal();
                    taken.insert(out[i].absolute.generic_string());
                }
            }
            for (size_t i = 0; i < plan.tilesets.size(); i++) {
                if (!out[i].absolute.empty()) continue;
                const std::string stem = sanitizedFileName(plan.tilesets[i].data.name);
                fs::path candidate = (dir / (stem + ".tileset")).lexically_normal();
                for (int suffix = 2; taken.count(candidate.generic_string()) || fs::exists(candidate); suffix++) {
                    candidate = (dir / (stem + "_" + std::to_string(suffix) + ".tileset")).lexically_normal();
                }
                taken.insert(candidate.generic_string());
                out[i].absolute = candidate;
                out[i].assetPath = project->normalizeToAssetsRelative(candidate).generic_string();
            }
            return out;
        }

        // The tileset document, its images assets-relative and by GUID (they may move later)
        std::string tilesetDocument(Project* project, const ImportTileset& tileset, const fs::path& assets, const fs::path& mapDir, const ImportPlan& plan,
                                    std::vector<std::string>& warnings) {
            TilesetData data = tileset.data;
            for (TilesetSource& source : data.sources) {
                // an Aseprite plan's source is an assets-relative path already
                if (source.asset.path.empty() || plan.format == "aseprite") continue;
                const fs::path absolute = (mapDir / source.asset.path).lexically_normal();
                if (!isBeneath(absolute, assets)) warnings.push_back("Tileset '" + data.name + "': the image " + source.asset.path + " is outside the project's assets");
                source.asset = absolute.lexically_normal().lexically_relative(assets.lexically_normal()).generic_string();
            }
            std::vector<TilesetProblem> problems;
            tileset::validate(data, problems);
            for (const TilesetProblem& problem : problems) warnings.push_back("Tileset '" + data.name + "': " + problem.field + ": " + problem.message);
            tilemap::stampSourceGuids(project, data);
            return tileset::write(data);
        }

        std::vector<const ImportGrid*> selectedGrids(const ImportPlan& plan, const std::string& name) {
            std::vector<const ImportGrid*> out;
            for (const ImportGrid& grid : plan.grids) {
                if (name.empty() || grid.name == name) out.push_back(&grid);
            }
            return out;
        }

        Command* writeTilesetsCommand(Project* project, std::vector<WriteFilesCmd::Item> items, const std::string& name) {
            auto command = std::make_unique<WriteFilesCmd>(std::move(items), [project](const std::vector<fs::path>& changed) {
                ProjectFileWrites::filesWritten(project, changed);
                for (const fs::path& path : changed) {
                    TilesetPool::invalidate(path.generic_string());
                    TilesetPool::invalidate(project->normalizeToAssetsRelative(path).generic_string());
                }
            });
            command->setName(name);
            return command.release();
        }

        // Runs commands into one undo step, undoing them all when one fails
        class ImportStep {
        public:
            ImportStep(CommandHistory* history): history(history), start(history->mark()) {}
            bool add(Command* command) { return history->addCommandNoMerge(command); }
            void rollback() { while (history->position() > start && history->undo()) {} }
            bool changed() const { return history->position() > start; }
            void commit(const std::string& label) { history->groupSince(start, label); }

        private:
            CommandHistory* history;
            size_t start;
        };

        // the world units (pixels / Pixels Per Unit) the Grid and layer fields hold, as the
        // legacy conversion stores them
        double worldUnits(double pixels) {
            return pixels / static_cast<double>(Engine::getPixelsPerUnit());
        }

        void addGridFields(MultiPropertyCmd& cmd, Project* project, uint32_t sceneId, Entity entity, const ImportGrid& grid, const std::string& importSource, bool linked) {
            const float ppu = static_cast<float>(Engine::getPixelsPerUnit());
            const auto add = [&](auto value, const char* name) { cmd.addPropertyCmd(project, sceneId, entity, ComponentType::GridComponent, name, value); };
            add(grid.spec.layout, "layout");
            add(Vector2(static_cast<float>(grid.spec.cellWidth) / ppu, static_cast<float>(grid.spec.cellHeight) / ppu), "cellSize");
            add(grid.spec.staggerAxis, "staggerAxis");
            add(grid.spec.staggerIndex, "staggerIndex");
            add(static_cast<float>(grid.spec.hexSideLength) / ppu, "hexSideLength");
            cmd.addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::GridComponent, "importSource", importSource);
            add(linked, "linked");
            cmd.addPropertyCmd<Vector3>(project, sceneId, entity, ComponentType::Transform, "position",
                                        Vector3(static_cast<float>(worldUnits(grid.originX)), static_cast<float>(worldUnits(grid.originY)), 0.0f));
        }

        void addLayerFields(MultiPropertyCmd& cmd, Project* project, uint32_t sceneId, Entity entity, const ImportLayer& layer, const std::string& tilesetFile, int order) {
            cmd.addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::TilemapLayerComponent, "tileset", tilesetFile);
            cmd.addPropertyCmd<bool>(project, sceneId, entity, ComponentType::TilemapLayerComponent, "enabled", layer.visible);
            cmd.addPropertyCmd<Vector4>(project, sceneId, entity, ComponentType::TilemapLayerComponent, "tint", Vector4(1.0f, 1.0f, 1.0f, layer.opacity));
            cmd.addPropertyCmd<Vector3>(project, sceneId, entity, ComponentType::Transform, "position",
                                        Vector3(static_cast<float>(worldUnits(layer.offsetX)), static_cast<float>(worldUnits(layer.offsetY)), 0.0f));
            cmd.addPropertyCmd<int32_t>(project, sceneId, entity, ComponentType::MeshComponent, "sortingOrder", order);
        }

        std::vector<TileCellChange> changesFromEmpty(const std::vector<ImportCell>& cells) {
            std::vector<TileCellChange> changes;
            changes.reserve(cells.size());
            for (const ImportCell& cell : cells) {
                TileCellChange change;
                change.cell = CellCoord{cell.x, cell.y};
                change.newValue = cell.value;
                change.newTerrain = cell.terrain;
                changes.push_back(change);
            }
            return changes;
        }

        std::vector<TileCellChange> changesFrom(const TileCells& current, const std::vector<ImportCell>& cells) {
            std::vector<TileCellChange> changes;
            std::set<std::pair<int32_t, int32_t>> planned;
            for (const ImportCell& cell : cells) {
                planned.insert({cell.y, cell.x});
                const CellCoord coord{cell.x, cell.y};
                const uint32_t oldValue = current.get(coord);
                const uint8_t oldTerrain = current.getTerrainValue(coord);
                if (oldValue == cell.value && oldTerrain == cell.terrain) continue;
                TileCellChange change;
                change.cell = coord;
                change.oldValue = oldValue;
                change.newValue = cell.value;
                change.oldTerrain = oldTerrain;
                change.newTerrain = cell.terrain;
                changes.push_back(change);
            }
            // what the file no longer has
            current.forEachUsed([&](CellCoord coord, uint32_t value, uint8_t terrain) {
                if (planned.count({coord.y, coord.x})) return;
                TileCellChange change;
                change.cell = coord;
                change.oldValue = value;
                change.oldTerrain = terrain;
                changes.push_back(change);
            });
            return changes;
        }

        std::string mapSourcePath(Project* project, const fs::path& mapFile) {
            return project->normalizeToAssetsRelative(mapFile).generic_string();
        }

        const char* const kMapExtensions[] = {".tmj", ".tmx", ".ldtk"};
        const char* const kDependencyExtensions[] = {".tsj", ".tsx", ".ldtkl"};

        bool extensionIn(const fs::path& path, const char* const* list, size_t count) {
            const std::string ext = lowerExtension(path);
            return std::any_of(list, list + count, [&](const char* e) { return ext == e; });
        }

    }

    bool loadPlan(Project* project, const fs::path& mapFile, ImportPlan& out, std::vector<ImportProblem>& problems, std::vector<fs::path>* filesRead) {
        std::string text;
        if (!readTextFile(mapFile, text)) {
            ImportProblem p;
            p.message = "cannot read " + mapFile.generic_string();
            p.file = mapFile.generic_string();
            p.fix = "check the file exists";
            problems.push_back(std::move(p));
            return false;
        }
        if (filesRead) filesRead->push_back(mapFile.lexically_normal());
        const fs::path dir = mapFile.parent_path();
        const fs::path assets = project ? project->getAssetsPath() : dir;
        const FileReader reader = [dir, assets, filesRead](const std::string& relative, std::string& data) {
            const fs::path path = (dir / relative).lexically_normal();
            if (!isBeneath(path, assets) || !readTextFile(path, data)) return false;
            if (filesRead) filesRead->push_back(path);
            return true;
        };
        const std::string name = mapFile.filename().generic_string();
        return lowerExtension(mapFile) == ".ldtk" ? parseLdtk(text, name, reader, out, problems) : parseTiled(text, name, reader, out, problems);
    }

    ImportReport previewReport(const ImportPlan& plan, const ImportTarget& target) {
        ImportReport report;
        report.counts = plan.counts();
        report.skipped = plan.skipped;
        report.warnings = plan.warnings;
        for (const ImportGrid* grid : selectedGrids(plan, target.grid)) {
            ImportReport::Grid g;
            g.name = grid->name;
            for (const ImportLayer& layer : grid->layers) g.layers.push_back(layer.name);
            report.grids.push_back(std::move(g));
        }
        if (target.project) {
            const std::vector<TilesetFile> files = tilesetFiles(plan, target, {});
            for (size_t i = 0; i < files.size(); i++) report.tilesets.push_back({plan.tilesets[i].data.name, files[i].assetPath});
        }
        return report;
    }

    ImportResult apply(const ImportPlan& plan, const ImportTarget& target) {
        ImportResult result;
        result.warnings = plan.warnings;
        Project* project = target.project;
        SceneProject* sceneProject = project ? project->getScene(target.sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene) return applyFailure(std::move(result), "the target scene is not open", "open the scene, then import again");
        const std::vector<const ImportGrid*> grids = selectedGrids(plan, target.grid);
        if (grids.empty()) return applyFailure(std::move(result), "the plan has no Grid to make" + (target.grid.empty() ? std::string() : " named '" + target.grid + "'"), "choose an existing level");
        if (!target.prefab.empty() && grids.size() > 1) return applyFailure(std::move(result), "a prefab holds one Grid; this map has several", "choose one level with `grid`");
        std::error_code ec;
        if (!target.prefab.empty() && fs::exists(target.prefab, ec)) return applyFailure(std::move(result), "a file already exists at " + target.prefab.generic_string(), "pick another prefab path");

        const fs::path mapDir = target.mapFile.parent_path();
        const fs::path assets = project->getAssetsPath();
        std::vector<TilesetFile> files = tilesetFiles(plan, target, {});
        std::vector<WriteFilesCmd::Item> items;
        for (size_t i = 0; i < files.size(); i++) {
            items.push_back({files[i].absolute, tilesetDocument(project, plan.tilesets[i], assets, mapDir, plan, result.warnings)});
            result.tilesetFiles.push_back(files[i].assetPath);
        }

        CommandHistory* history = CommandHandle::get(target.sceneId);
        ImportStep step(history);
        const auto fail = [&](const char* what) {
            step.rollback();
            return applyFailure(std::move(result), std::string("the import could not ") + what, "check the project's files, then try again");
        };
        if (!items.empty() && !step.add(writeTilesetsCommand(project, std::move(items), "Write Imported Tilesets"))) return fail("write the tileset files");

        const std::string importSource = target.linked ? mapSourcePath(project, target.mapFile) : std::string();
        for (const ImportGrid* grid : grids) {
            auto* createGrid = new CreateEntityCmd(project, target.sceneId, grid->name, EntityCreationType::TILEMAP_GRID, target.parent);
            createGrid->setQuiet(true);
            if (!step.add(createGrid)) return fail("create the Grid");
            const Entity gridEntity = createGrid->getEntity();
            result.grids.push_back(gridEntity);

            auto gridFields = std::make_unique<MultiPropertyCmd>();
            addGridFields(*gridFields, project, target.sceneId, gridEntity, *grid, importSource, target.linked);
            if (!step.add(gridFields.release())) return fail("set the Grid's fields");

            for (size_t order = 0; order < grid->layers.size(); order++) {
                const ImportLayer& layer = grid->layers[order];
                auto* createLayer = new CreateEntityCmd(project, target.sceneId, layer.name, EntityCreationType::TILEMAP_LAYER, gridEntity);
                createLayer->setQuiet(true);
                if (!step.add(createLayer)) return fail("create a layer");
                const Entity layerEntity = createLayer->getEntity();
                result.layers.push_back(layerEntity);

                const std::string tilesetFile = layer.tileset >= 0 ? files[static_cast<size_t>(layer.tileset)].assetPath : std::string();
                auto layerFields = std::make_unique<MultiPropertyCmd>();
                addLayerFields(*layerFields, project, target.sceneId, layerEntity, layer, tilesetFile, static_cast<int>(order));
                if (!step.add(layerFields.release())) return fail("set a layer's fields");
                if (!layer.cells.empty() && !step.add(new tilemap::TilemapCellsCmd(project, target.sceneId, layerEntity, changesFromEmpty(layer.cells), "Import Tilemap Cells"))) return fail("write a layer's cells");
            }
        }

        if (!target.prefab.empty()) {
            const fs::path relative = project->normalizeToProjectRelative(target.prefab);
            FileUtils::createDirectories(target.prefab.parent_path(), ec);
            SceneProject* current = project->getScene(target.sceneId);
            const Entity root = result.grids.front();
            const YAML::Node node = Stream::encodeEntity(root, current->scene, project, current);
            if (!step.add(new CreatePrefabCmd(project, target.sceneId, relative, node))) return fail("write the prefab");
            if (!step.add(new DeleteEntityCmd(project, target.sceneId, root, true))) return fail("remove the build GameObjects");
            result.prefab = relative.generic_string();
            result.grids.clear();
            result.layers.clear();
        }

        step.commit("Import Tilemap " + target.mapFile.filename().generic_string());
        result.ok = true;
        return result;
    }

    ImportResult reimportLinked(Project* project, uint32_t sceneId, Entity gridEntity) {
        ImportResult result;
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        GridComponent* gridNow = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<GridComponent>(gridEntity) : nullptr;
        if (!gridNow || !gridNow->linked || gridNow->importSource.path.empty()) return applyFailure(std::move(result), "the GameObject is not a Grid linked to an imported map", "import the map with linked on");
        Scene* scene = sceneProject->scene;

        const std::string sourcePath = gridNow->importSource.path;
        const fs::path mapFile = (project->getAssetsPath() / sourcePath).lexically_normal();
        ImportPlan plan;
        std::vector<ImportProblem> problems;
        if (!loadPlan(project, mapFile, plan, problems)) {
            result.ok = false;
            result.problem = problems.empty() ? ImportProblem() : problems.front();
            return result;
        }
        result.warnings = plan.warnings;

        const std::string gridName = scene->getEntityName(gridEntity);
        const ImportGrid* grid = nullptr;
        if (plan.grids.size() == 1) grid = &plan.grids.front();
        else {
            for (const ImportGrid& candidate : plan.grids) {
                if (candidate.name == gridName) grid = &candidate;
            }
        }
        if (!grid) {
            result.problem.kind = "grid_not_found";
            result.problem.message = "the map has no level named '" + gridName + "'";
            result.problem.fix = "rename the Grid after its level, or unlink it";
            return result;
        }

        // the layer GameObjects by name
        std::map<std::string, Entity> layerEntities;
        for (const Entity entity : sceneProject->entities) {
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (transform && transform->parent == gridEntity && scene->findComponent<TilemapLayerComponent>(entity)) layerEntities[scene->getEntityName(entity)] = entity;
        }

        // a tileset a matched layer already uses keeps its file (overwritten in place)
        std::map<int, std::string> kept;
        std::set<int> usedTilesets;
        for (const ImportLayer& layer : grid->layers) {
            if (layer.tileset < 0) continue;
            usedTilesets.insert(layer.tileset);
            const auto entity = layerEntities.find(layer.name);
            if (entity == layerEntities.end()) continue;
            const std::string& path = scene->getComponent<TilemapLayerComponent>(entity->second).tileset.path;
            if (!path.empty() && !kept.count(layer.tileset)) kept[layer.tileset] = path;
        }

        ImportTarget target;
        target.project = project;
        target.sceneId = sceneId;
        target.mapFile = mapFile;
        const std::vector<TilesetFile> files = tilesetFiles(plan, target, kept);
        const fs::path assets = project->getAssetsPath();
        std::vector<WriteFilesCmd::Item> items;
        for (int index : usedTilesets) {
            const size_t i = static_cast<size_t>(index);
            std::string document = tilesetDocument(project, plan.tilesets[i], assets, mapFile.parent_path(), plan, result.warnings);
            std::string existing;
            if (!readTextFile(files[i].absolute, existing) || existing != document) items.push_back({files[i].absolute, std::move(document)});
            result.tilesetFiles.push_back(files[i].assetPath);
        }

        CommandHistory* history = CommandHandle::get(sceneId);
        ImportStep step(history);
        const auto fail = [&](const char* what) {
            step.rollback();
            return applyFailure(std::move(result), std::string("the re-import could not ") + what, "check the project's files, then try again");
        };
        if (!items.empty() && !step.add(writeTilesetsCommand(project, std::move(items), "Write Imported Tilesets"))) return fail("write the tileset files");

        // the Grid's layout: only what changed, so an unchanged map adds no undo step
        const float ppu = static_cast<float>(Engine::getPixelsPerUnit());
        const Vector2 cellSize(static_cast<float>(grid->spec.cellWidth) / ppu, static_cast<float>(grid->spec.cellHeight) / ppu);
        const float hexSide = static_cast<float>(grid->spec.hexSideLength) / ppu;
        const GridComponent gridValues = *gridNow;
        if (gridValues.layout != grid->spec.layout || gridValues.cellSize != cellSize || gridValues.staggerAxis != grid->spec.staggerAxis || gridValues.staggerIndex != grid->spec.staggerIndex || gridValues.hexSideLength != hexSide) {
            auto fields = std::make_unique<MultiPropertyCmd>();
            fields->addPropertyCmd<TileGridLayout>(project, sceneId, gridEntity, ComponentType::GridComponent, "layout", grid->spec.layout);
            fields->addPropertyCmd<Vector2>(project, sceneId, gridEntity, ComponentType::GridComponent, "cellSize", cellSize);
            fields->addPropertyCmd<StaggerAxis>(project, sceneId, gridEntity, ComponentType::GridComponent, "staggerAxis", grid->spec.staggerAxis);
            fields->addPropertyCmd<StaggerIndex>(project, sceneId, gridEntity, ComponentType::GridComponent, "staggerIndex", grid->spec.staggerIndex);
            fields->addPropertyCmd<float>(project, sceneId, gridEntity, ComponentType::GridComponent, "hexSideLength", hexSide);
            if (!step.add(fields.release())) return fail("update the Grid");
        }

        std::set<std::string> planNames;
        for (size_t order = 0; order < grid->layers.size(); order++) {
            const ImportLayer& layer = grid->layers[order];
            planNames.insert(layer.name);
            const std::string tilesetFile = layer.tileset >= 0 ? files[static_cast<size_t>(layer.tileset)].assetPath : std::string();
            Entity layerEntity = NULL_ENTITY;
            std::vector<TileCellChange> changes;
            const auto existing = layerEntities.find(layer.name);
            if (existing != layerEntities.end()) {
                layerEntity = existing->second;
                result.layers.push_back(layerEntity);
                const TilemapLayerComponent& current = scene->getComponent<TilemapLayerComponent>(layerEntity);
                const Transform* transform = scene->findComponent<Transform>(layerEntity);
                const Vector3 position(static_cast<float>(worldUnits(layer.offsetX)), static_cast<float>(worldUnits(layer.offsetY)), 0.0f);
                const Vector4 tint(1.0f, 1.0f, 1.0f, layer.opacity);
                if (current.tileset.path != tilesetFile || current.enabled != layer.visible || current.tint != tint || (tilemapmesh::hasMesh(scene, layerEntity) && tilemapmesh::sortingOrder(scene, layerEntity) != static_cast<int>(order)) || (transform && transform->position != position)) {
                    auto fields = std::make_unique<MultiPropertyCmd>();
                    addLayerFields(*fields, project, sceneId, layerEntity, layer, tilesetFile, static_cast<int>(order));
                    if (!step.add(fields.release())) return fail("update a layer");
                }
                changes = changesFrom(current.cells.read(), layer.cells);
            } else {
                auto* createLayer = new CreateEntityCmd(project, sceneId, layer.name, EntityCreationType::TILEMAP_LAYER, gridEntity);
                createLayer->setQuiet(true);
                if (!step.add(createLayer)) return fail("create a layer");
                layerEntity = createLayer->getEntity();
                result.layers.push_back(layerEntity);
                auto fields = std::make_unique<MultiPropertyCmd>();
                addLayerFields(*fields, project, sceneId, layerEntity, layer, tilesetFile, static_cast<int>(order));
                if (!step.add(fields.release())) return fail("set a layer's fields");
                changes = changesFromEmpty(layer.cells);
            }
            if (!changes.empty() && !step.add(new tilemap::TilemapCellsCmd(project, sceneId, layerEntity, std::move(changes), "Reimport Tilemap Cells"))) return fail("write a layer's cells");
        }

        for (const auto& [name, entity] : layerEntities) {
            if (planNames.count(name)) continue;
            if (!step.add(new DeleteEntityCmd(project, sceneId, entity, true))) return fail("remove a layer");
            result.removedLayers.push_back(entity);
        }

        if (step.changed()) step.commit("Reimport " + mapFile.filename().generic_string());
        result.ok = true;
        return result;
    }

    void linkedSourcesChanged(Project& project, const std::vector<fs::path>& changed) {
        std::set<fs::path> files;
        bool anyMap = false, anyDependency = false;
        for (const fs::path& path : changed) {
            if (extensionIn(path, kMapExtensions, std::size(kMapExtensions))) anyMap = true;
            else if (extensionIn(path, kDependencyExtensions, std::size(kDependencyExtensions))) anyDependency = true;
            else continue;
            files.insert(path.lexically_normal());
        }
        if (!anyMap && !anyDependency) return;

        std::vector<std::pair<uint32_t, Entity>> linked;
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            for (const Entity entity : sceneProject->entities) {
                const GridComponent* grid = sceneProject->scene->findComponent<GridComponent>(entity);
                if (!grid || !grid->linked || grid->importSource.path.empty()) continue;
                PrefabObjectRef ref;
                if (project.findPrefabObject(sceneProject->id, entity, ref)) continue;   // an instance follows its prefab
                linked.emplace_back(sceneProject->id, entity);
            }
        }

        for (const auto& [sceneId, entity] : linked) {
            SceneProject* sceneProject = project.getScene(sceneId);
            const GridComponent* grid = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<GridComponent>(entity) : nullptr;
            if (!grid) continue;
            const fs::path source = (project.getAssetsPath() / grid->importSource.path).lexically_normal();
            bool affected = files.count(source) > 0;
            if (!affected && anyDependency) {
                // an external tileset or level of this map?
                ImportPlan plan;
                std::vector<ImportProblem> problems;
                std::vector<fs::path> read;
                loadPlan(&project, source, plan, problems, &read);
                affected = std::any_of(read.begin(), read.end(), [&](const fs::path& p) { return files.count(p) > 0; });
            }
            if (!affected) continue;
            const ImportResult result = reimportLinked(&project, sceneId, entity);
            if (result.ok) Out::info("[Tilemap] Re-imported %s", grid->importSource.path.c_str());
            else Out::warning("[Tilemap] %s: %s (kept the previous version)", grid->importSource.path.c_str(), result.problem.message.c_str());
        }
    }

}
