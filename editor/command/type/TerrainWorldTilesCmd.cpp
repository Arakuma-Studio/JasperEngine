// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainWorldTilesCmd.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::terraincore;

namespace {

    std::string worldTileText(TileCoord tile) {
        return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ")";
    }

}

editor::TerrainWorldTilesCmd::TerrainWorldTilesCmd(Project* project, uint32_t sceneId, Entity entity, std::string verb, std::vector<TileCoord> tiles,
                                                   Change change)
    : project(project), sceneId(sceneId), entity(entity), verb(std::move(verb)), tiles(std::move(tiles)), change(std::move(change)) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (sceneProject && sceneProject->scene) name = sceneProject->scene->getEntityName(entity);
}

TerrainWorldRuntime* editor::TerrainWorldTilesCmd::runtime(std::string& error) const {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainWorldSystem* system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<TerrainWorldSystem>().get() : nullptr;
    if (!system) {
        error = "The scene of the Terrain World is not open.";
        return nullptr;
    }
    std::string why;
    TerrainWorldRuntime* world = system->ensureRuntime(entity, &why);
    if (!world || !world->started()) {
        error = "The Terrain World is not running: " + (why.empty() ? std::string("its data did not load") : why) + ".";
        return nullptr;
    }
    if (world->playing()) {
        error = "A Terrain World's tiles change only outside Play: stop Play first.";
        return nullptr;
    }
    return world;
}

void editor::TerrainWorldTilesCmd::put(TerrainWorldRuntime& world, bool after) {
    TiledMapStore& store = world.store();
    for (const Snapshot& snapshot : snapshots) {
        const std::shared_ptr<const WorldTile>& data = after ? snapshot.after : snapshot.before;
        if (data) {
            // the store pins and edits what it is given: the snapshot stays as it was
            store.restoreTile(snapshot.tile, std::make_shared<WorldTile>(*data));
        } else if (world.present(snapshot.tile)) {
            store.dropTile(snapshot.tile);
        }
    }
    std::string error;
    if (!TerrainWorldFiles::flushWorld(world, &error) && !error.empty()) Out::error("Terrain World: %s", error.c_str());
}

bool editor::TerrainWorldTilesCmd::execute() {
    lastError.clear();
    TerrainWorldRuntime* world = runtime(lastError);
    if (!world) return false;
    if (auto* source = dynamic_cast<EditorWorldSource*>(&world->source())) folder = FileUtils::pathToUtf8(source->folder());
    if (done) {
        put(*world, true);
        return true;
    }
    snapshots.clear();
    for (const TileCoord& tile : tiles) {
        Snapshot snapshot;
        snapshot.tile = tile;
        if (world->present(tile)) {
            std::shared_ptr<const WorldTile> current = world->tile(tile);
            if (!current) {
                lastError = "Tile " + worldTileText(tile) + " could not be read; nothing was changed.";
                return false;
            }
            snapshot.before = std::make_shared<WorldTile>(*current);
        }
        snapshots.push_back(std::move(snapshot));
    }
    if (!change || !change(*world, lastError)) {
        if (lastError.empty()) lastError = verb + " changed nothing.";
        // whatever the change did before it stopped goes back
        put(*world, false);
        return false;
    }
    // the after side holds the derived sections the files will: a redo writes it as it is
    world->settleEdits();
    bool any = false;
    for (Snapshot& snapshot : snapshots) {
        if (world->present(snapshot.tile)) {
            std::shared_ptr<const WorldTile> current = world->tile(snapshot.tile);
            if (current) snapshot.after = std::make_shared<WorldTile>(*current);
        }
        any = any || static_cast<bool>(snapshot.before) != static_cast<bool>(snapshot.after) ||
              (snapshot.before && snapshot.after && snapshot.before->height.bytes != snapshot.after->height.bytes);
    }
    noOp = !any;
    done = true;
    std::string error;
    if (!TerrainWorldFiles::flushWorld(*world, &error) && !error.empty()) Out::error("Terrain World: %s", error.c_str());
    return true;
}

void editor::TerrainWorldTilesCmd::undo() {
    std::string error;
    TerrainWorldRuntime* world = runtime(error);
    if (!world) {
        undoProblem = error;
        return;
    }
    put(*world, false);
}

std::vector<editor::Command::Touched> editor::TerrainWorldTilesCmd::touchedPaths(bool) const {
    std::vector<Touched> touched;
    if (folder.empty()) return touched;
    const std::filesystem::path root = FileUtils::pathFromUtf8(folder);
    for (const Snapshot& snapshot : snapshots) touched.push_back(Touched{root / FileUtils::pathFromUtf8(terrainWorldTileName(snapshot.tile)), true});
    return touched;
}

std::string editor::TerrainWorldTilesCmd::describe() const {
    return verb + " (" + (name.empty() ? std::string("Terrain World") : name) + ")";
}

size_t editor::TerrainWorldTilesCmd::memoryBytes() const {
    size_t bytes = sizeof(*this);
    for (const Snapshot& snapshot : snapshots) {
        if (snapshot.before) bytes += snapshot.before->cpuBytes();
        if (snapshot.after) bytes += snapshot.after->cpuBytes();
    }
    return bytes;
}

bool editor::checkTerrainWorldTiles(TerrainWorldRuntime& runtime, const std::vector<TileCoord>& tiles, bool remove, std::vector<TileCoord>& changed,
                                    std::string& error) {
    changed.clear();
    if (tiles.empty()) {
        error = "No tiles were given: list them as [[x, z], ...].";
        return false;
    }
    if (!runtime.started()) {
        error = "The Terrain World is not running; its data did not load.";
        return false;
    }
    std::set<uint32_t> seen;
    TileCoord low, high;
    bool box = runtime.bounds(low, high);
    for (const TileCoord& tile : tiles) {
        if (!seen.insert(tile.key()).second) continue;
        if (remove) {
            if (!runtime.present(tile)) {
                error = "Tile " + worldTileText(tile) + " is not in the world: sample_terrain {world} lists its tiles. Nothing was changed.";
                return false;
            }
            changed.push_back(tile);
            continue;
        }
        if (runtime.present(tile)) continue;
        std::string why;
        if (!runtime.store().canCreate(tile, &why)) {
            error = "Tile " + worldTileText(tile) + " cannot be added: " + why + ". Nothing was changed.";
            return false;
        }
        // canCreate measures one tile against the present ones; the call's tiles count together
        if (box) {
            low = TileCoord(std::min<int>(low.x, tile.x), std::min<int>(low.z, tile.z));
            high = TileCoord(std::max<int>(high.x, tile.x), std::max<int>(high.z, tile.z));
        } else {
            low = high = tile;
            box = true;
        }
        const int maxBox = runtime.store().options().maxBoxTiles;
        if (high.x - low.x + 1 > maxBox || high.z - low.z + 1 > maxBox) {
            error = "These tiles would make the world span more than " + std::to_string(maxBox) +
                    " tiles a side; add fewer, closer to the others. Nothing was changed.";
            return false;
        }
        changed.push_back(tile);
    }
    if (changed.empty()) {
        error = "Every tile given is in the world already; nothing was changed.";
        return false;
    }
    return true;
}

bool editor::editTerrainWorldTiles(Project* project, uint32_t sceneId, Entity world, const std::vector<TileCoord>& tiles, bool remove, std::string& error,
                                   std::vector<TileCoord>* changedOut) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainWorldSystem* system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<TerrainWorldSystem>().get() : nullptr;
    if (!system) {
        error = "The scene is not open.";
        return false;
    }
    std::string why;
    TerrainWorldRuntime* runtime = system->ensureRuntime(world, &why);
    if (!runtime) {
        error = "\"" + sceneProject->scene->getEntityName(world) + "\" has no running Terrain World: " + why + ".";
        return false;
    }
    if (runtime->playing()) {
        error = "A Terrain World's tiles change only outside Play: stop Play first.";
        return false;
    }
    std::vector<TileCoord> changed;
    if (!checkTerrainWorldTiles(*runtime, tiles, remove, changed, error)) return false;
    // the history frees a command that failed: its reason comes back through this
    auto reason = std::make_shared<std::string>();
    auto change = [changed, remove, reason](TerrainWorldRuntime& target, std::string& failure) {
        for (const TileCoord& tile : changed) {
            if (remove) {
                target.store().removeTile(tile);
            } else if (!target.store().addTile(tile, &failure)) {
                failure = "Tile " + worldTileText(tile) + " cannot be added: " + failure + ". Nothing was changed.";
                *reason = failure;
                return false;
            }
        }
        return true;
    };
    auto* command = new TerrainWorldTilesCmd(project, sceneId, world, remove ? "Remove Tiles" : "Add Tiles", changed, change);
    command->setNoMerge();
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!history->addCommandNoMerge(command)) {
        error = reason->empty() ? std::string(remove ? "The tiles could not be removed." : "The tiles could not be added.") : *reason;
        return false;
    }
    if (changedOut) *changedOut = changed;
    return true;
}
