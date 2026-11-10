// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainWorldImportCmd.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/FileUtils.h"

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::terraincore;

namespace fs = std::filesystem;

namespace {

    std::string importTileText(TileCoord tile) {
        return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ")";
    }

    bool readStashed(const fs::path& path, std::string& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        std::ostringstream bytes;
        bytes << file.rdbuf();
        out = bytes.str();
        return static_cast<bool>(file) || file.eof();
    }

}

editor::TerrainWorldImportCmd::Stamp editor::TerrainWorldImportCmd::stampOf(const fs::path& file) {
    Stamp stamp;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return stamp;
    stamp.exists = true;
    stamp.size = fs::file_size(file, ec);
    stamp.time = fs::last_write_time(file, ec);
    return stamp;
}

editor::TerrainWorldImportCmd::TerrainWorldImportCmd(Project* project, uint32_t sceneId, Entity entity, fs::path stash, std::vector<TileCoord> tiles,
                                                     std::vector<Stamp> stamps, std::shared_ptr<std::string> reason)
    : project(project), sceneId(sceneId), entity(entity), stash(std::move(stash)), tiles(std::move(tiles)), stamps(std::move(stamps)),
      reason(std::move(reason)) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (sceneProject && sceneProject->scene) name = sceneProject->scene->getEntityName(entity);
}

editor::TerrainWorldImportCmd::~TerrainWorldImportCmd() {
    // the stash is the step's own: it goes with it
    std::error_code ec;
    if (!stash.empty()) fs::remove_all(stash, ec);
}

TerrainWorldRuntime* editor::TerrainWorldImportCmd::runtime(std::string& error) const {
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

bool editor::TerrainWorldImportCmd::put(TerrainWorldRuntime& world, bool after, std::string& error) {
    auto write = [&](size_t i, bool side, std::string& why) {
        const TileCoord tile = tiles[i];
        if (!side && !existed[i]) {
            if (!removeTileFile(folder, tile, why)) return false;
            world.fileChanged(tile, false);
            return true;
        }
        std::string bytes;
        const fs::path from = stash / (side ? "after" : "before") / FileUtils::pathFromUtf8(terrainWorldTileName(tile));
        if (!readStashed(from, bytes)) {
            why = "the kept tile " + importTileText(tile) + " is gone from " + FileUtils::pathToUtf8(from);
            return false;
        }
        if (!writeTileBytes(folder, tile, bytes, why)) return false;
        world.fileChanged(tile, true);
        return true;
    };
    for (size_t i = 0; i < tiles.size(); i++) {
        if (write(i, after, error)) continue;
        // what was written goes back, so a failed step leaves the world as it was
        for (size_t j = 0; j < i; j++) {
            std::string ignored;
            write(j, !after, ignored);
        }
        return false;
    }
    std::string why;
    if (!TerrainWorldFiles::flushWorld(world, &why) && !why.empty()) Out::error("Terrain World: %s", why.c_str());
    return true;
}

bool editor::TerrainWorldImportCmd::execute() {
    const bool ok = apply();
    if (!ok && reason) *reason = lastError;
    return ok;
}

bool editor::TerrainWorldImportCmd::apply() {
    lastError.clear();
    TerrainWorldRuntime* world = runtime(lastError);
    if (!world) return false;
    auto* source = dynamic_cast<EditorWorldSource*>(&world->source());
    if (!source) {
        lastError = "The Terrain World's folder is not in the project.";
        return false;
    }
    folder = source->folder();
    if (done) {
        if (!put(*world, true, lastError)) return false;
        return true;
    }
    // the job read the files as they were flushed when it started: an edit since then would be lost
    std::string why;
    TerrainWorldFiles::flushWorld(*world, &why);
    existed.assign(tiles.size(), 0);
    for (size_t i = 0; i < tiles.size(); i++) {
        const fs::path file = folder / FileUtils::pathFromUtf8(terrainWorldTileName(tiles[i]));
        if (i >= stamps.size() || !(stampOf(file) == stamps[i])) {
            lastError = "Tile " + importTileText(tiles[i]) + " of the Terrain World changed while the heightmap was read: import it again. Nothing was changed.";
            return false;
        }
        existed[i] = stamps[i].exists ? 1 : 0;
        if (!existed[i]) continue;
        // the replaced file is kept as it is: a new name for it when the disk allows, else a copy
        const fs::path kept = stash / "before" / FileUtils::pathFromUtf8(terrainWorldTileName(tiles[i]));
        std::error_code ec;
        fs::create_directories(kept.parent_path(), ec);
        fs::create_hard_link(file, kept, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(file, kept, fs::copy_options::none, ec);
        }
        if (ec) {
            lastError = "Tile " + importTileText(tiles[i]) + " could not be kept for undo (" + ec.message() + "). Nothing was changed.";
            return false;
        }
    }
    if (!put(*world, true, lastError)) {
        lastError += ". Nothing was changed.";
        return false;
    }
    done = true;
    return true;
}

void editor::TerrainWorldImportCmd::undo() {
    std::string error;
    TerrainWorldRuntime* world = runtime(error);
    if (!world) {
        undoProblem = error;
        return;
    }
    if (!put(*world, false, error)) undoProblem = "The heightmap import could not be undone: " + error + ".";
}

std::vector<editor::Command::Touched> editor::TerrainWorldImportCmd::touchedPaths(bool) const {
    std::vector<Touched> touched;
    if (folder.empty()) return touched;
    for (const TileCoord& tile : tiles) touched.push_back(Touched{folder / FileUtils::pathFromUtf8(terrainWorldTileName(tile)), true});
    return touched;
}

std::string editor::TerrainWorldImportCmd::describe() const {
    return "Import Heightmap (" + (name.empty() ? std::string("Terrain World") : name) + ")";
}
