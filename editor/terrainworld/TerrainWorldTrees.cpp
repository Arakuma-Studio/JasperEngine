// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldTrees.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "foliage/TreeDataFile.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrainworld/TerrainWorldEditor.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/FileUtils.h"

#include "zstd.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::terraincore;
namespace fs = std::filesystem;

namespace {

    constexpr uint64_t WORLD_TREE_RECORD_SPAN = uint64_t(1) << 31;

    bool editorWorldTreeDecompress(const void* input, size_t inputBytes, void* output, size_t outputBytes) {
        const size_t written = ZSTD_decompress(output, outputBytes, input, inputBytes);
        return !ZSTD_isError(written) && written == outputBytes;
    }

    std::string worldTreeTileText(TileCoord tile) {
        return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ")";
    }

    bool readWorldTreeFile(const fs::path& path, std::string& out) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        std::ostringstream bytes;
        bytes << file.rdbuf();
        out = bytes.str();
        return true;
    }

    // What a tile's file holds as a store: empty when there is none
    std::shared_ptr<TreeStore> worldTreeStoreOf(const std::optional<std::string>& bytes) {
        auto store = std::make_shared<TreeStore>();
        if (bytes) {
            std::string error;
            if (!store->decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size()), &error, editorWorldTreeDecompress)) {
                store->clear();
            }
        }
        return store;
    }

    // The World and its folder out of Play, or why not
    TerrainWorldRuntime* worldTreeRuntime(editor::Project* project, uint32_t sceneId, Entity entity, fs::path& folder, std::string& error) {
        editor::SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene) {
            error = "The scene of the Terrain World is not open.";
            return nullptr;
        }
        TerrainWorldRuntime* runtime = editor::terrainWorldRuntimeOf(sceneProject->scene, entity, error);
        if (!runtime) return nullptr;
        if (runtime->playing()) {
            error = "A Terrain World's trees change only outside Play: stop Play first.";
            return nullptr;
        }
        auto* source = dynamic_cast<editor::EditorWorldSource*>(&runtime->source());
        if (!source) {
            error = "The Terrain World's data is not a folder of the project.";
            return nullptr;
        }
        folder = source->folder();
        return runtime;
    }

    // One side of a tile's file: written or removed, the manifest told (a region made later loads it)
    bool putWorldTreeFile(TerrainWorldRuntime& runtime, const fs::path& folder, TileCoord tile, const std::optional<std::string>& bytes, std::string& error) {
        const bool ok = bytes ? editor::writeTreeBytes(folder, tile, *bytes, error) : editor::removeTreeFile(folder, tile, error);
        if (!ok) return false;
        std::vector<WorldTileInfo>& infos = runtime.manifest().tiles;
        auto info = std::find_if(infos.begin(), infos.end(), [&](const WorldTileInfo& entry) { return entry.coord == tile; });
        if (info == infos.end()) {
            WorldTileInfo added;
            added.coord = tile;
            infos.push_back(added);
            info = infos.end() - 1;
        }
        info->trees = bytes.has_value();
        return true;
    }

    // A resident tile's region draws what the file now holds
    void handWorldTreesToRegion(Scene* scene, Entity entity, TileCoord tile, std::shared_ptr<TreeStore> store) {
        FoliageSystem* foliage = scene ? scene->findSystem<FoliageSystem>().get() : nullptr;
        if (foliage && foliage->hasRegion(entity, tile.key())) foliage->treeStoreReplaced(entity, tile.key(), std::move(store));
    }

    Scene* worldTreeScene(editor::Project* project, uint32_t sceneId) {
        editor::SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        return sceneProject ? sceneProject->scene : nullptr;
    }

}

uint64_t jasperengine::editor::terrainWorldTreeId(TileCoord tile, uint32_t record) {
    const uint64_t cell = static_cast<uint64_t>(tile.x - WORLD_TILE_MIN) * WORLD_TILES_PER_SIDE + static_cast<uint64_t>(tile.z - WORLD_TILE_MIN);
    return cell * WORLD_TREE_RECORD_SPAN + (record % WORLD_TREE_RECORD_SPAN);
}

bool jasperengine::editor::splitTerrainWorldTreeId(uint64_t id, TileCoord& tile, uint32_t& record) {
    const uint64_t cell = id / WORLD_TREE_RECORD_SPAN;
    if (cell >= static_cast<uint64_t>(WORLD_TILES_PER_SIDE) * WORLD_TILES_PER_SIDE) return false;
    tile = TileCoord(static_cast<int>(cell / WORLD_TILES_PER_SIDE) + WORLD_TILE_MIN, static_cast<int>(cell % WORLD_TILES_PER_SIDE) + WORLD_TILE_MIN);
    record = static_cast<uint32_t>(id % WORLD_TREE_RECORD_SPAN);
    return record != 0;
}

// ------------------------------------------------------------------ TerrainWorldTreeEdit

editor::TerrainWorldTreeEdit::TerrainWorldTreeEdit(Project* project, uint32_t sceneId, Entity world)
    : project(project), sceneId(sceneId), entity(world) {
    this->world = worldTreeRuntime(project, sceneId, world, folder, problem);
}

float editor::TerrainWorldTreeEdit::tileSize() const {
    return world ? world->lattice().tileSize() : 0.0f;
}

float editor::TerrainWorldTreeEdit::maxHeight() const {
    return world ? world->maxHeight() : 0.0f;
}

TileCoord editor::TerrainWorldTreeEdit::tileAt(float localX, float localZ) const {
    const float size = std::max(tileSize(), 1.0f);
    return TileCoord(tileOfLocal(localX, size), tileOfLocal(localZ, size));
}

bool editor::TerrainWorldTreeEdit::present(TileCoord tile) const {
    return world && tile.addressable() && world->present(tile);
}

Vector2 editor::TerrainWorldTreeEdit::centre(TileCoord tile) const {
    const float size = tileSize();
    return Vector2((static_cast<float>(tile.x) + 0.5f) * size, (static_cast<float>(tile.z) + 0.5f) * size);
}

std::vector<TileCoord> editor::TerrainWorldTreeEdit::tilesIn(float minX, float minZ, float maxX, float maxZ) const {
    std::vector<TileCoord> found;
    if (!world) return found;
    const TileCoord low = tileAt(minX, minZ);
    const TileCoord high = tileAt(maxX, maxZ);
    for (int z = std::max(static_cast<int>(low.z), WORLD_TILE_MIN); z <= std::min(static_cast<int>(high.z), WORLD_TILE_MAX); z++) {
        for (int x = std::max(static_cast<int>(low.x), WORLD_TILE_MIN); x <= std::min(static_cast<int>(high.x), WORLD_TILE_MAX); x++) {
            if (present(TileCoord(x, z))) found.push_back(TileCoord(x, z));
        }
    }
    return found;
}

std::vector<TileCoord> editor::TerrainWorldTreeEdit::presentTiles() const {
    std::vector<TileCoord> found;
    TileCoord low, high;
    if (!world || !world->bounds(low, high)) return found;
    for (int z = low.z; z <= high.z; z++) {
        for (int x = low.x; x <= high.x; x++) {
            if (present(TileCoord(x, z))) found.push_back(TileCoord(x, z));
        }
    }
    return found;
}

editor::TerrainWorldTreeEdit::Tile* editor::TerrainWorldTreeEdit::tile(TileCoord coord, std::string* why) {
    if (!world) {
        if (why) *why = problem;
        return nullptr;
    }
    const auto found = tiles.find(coord.key());
    if (found != tiles.end()) {
        if (!found->second.refused.empty()) {
            if (why) *why = found->second.refused;
            return nullptr;
        }
        return &found->second;
    }
    if (!present(coord)) {
        if (why) *why = "there is no tile at " + worldTreeTileText(coord) + ": trees need ground";
        return nullptr;
    }

    Tile entry;
    entry.coord = coord;
    std::string bytes;
    if (readWorldTreeFile(folder / terrainWorldTreeName(coord), bytes)) entry.before = std::move(bytes);
    auto fromFile = std::make_shared<TreeStore>();
    if (entry.before) {
        std::string error;
        if (!fromFile->decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(entry.before->data()), entry.before->size()), &error,
                              editorWorldTreeDecompress)) {
            entry.refused = terrainWorldTreeName(coord) + " could not be read (" + error + "); it is not overwritten";
            if (why) *why = entry.refused;
            tiles[coord.key()] = std::move(entry);
            return nullptr;
        }
    }
    Scene* scene = worldTreeScene(project, sceneId);
    FoliageSystem* foliage = scene ? scene->findSystem<FoliageSystem>().get() : nullptr;
    if (foliage && foliage->hasRegion(entity, coord.key())) {
        // a region still loading, or one that came with no file, takes the file's store now: its own load is then ignored
        if (!foliage->getRegionTreeStore(entity, coord.key())) foliage->treeStoreReplaced(entity, coord.key(), fromFile);
        entry.store = foliage->getOrCreateTreeStore(entity, coord.key());
        entry.region = entry.store != nullptr;
    }
    if (!entry.store) entry.store = fromFile;
    return &(tiles[coord.key()] = std::move(entry));
}

const TreeStore* editor::TerrainWorldTreeEdit::trees(TileCoord coord, std::string* why) {
    Tile* entry = tile(coord, why);
    return entry ? entry->store.get() : nullptr;
}

bool editor::TerrainWorldTreeEdit::apply(TileCoord coord, std::span<const uint32_t> removed, std::span<const TreeRecord> added, TreeDelta* delta) {
    Tile* entry = tile(coord, nullptr);
    if (!entry) return false;
    TreeDelta local;
    entry->store->apply(removed, added, {}, &local);
    entry->changed = entry->changed || !local.empty();
    if (entry->region) {
        if (FoliageSystem* foliage = worldTreeScene(project, sceneId)->findSystem<FoliageSystem>().get()) foliage->treesChanged(entity, coord.key(), local);
    }
    if (delta) *delta = std::move(local);
    return true;
}

bool editor::TerrainWorldTreeEdit::applyDelta(TileCoord coord, const TreeDelta& delta) {
    Tile* entry = tile(coord, nullptr);
    if (!entry) return false;
    entry->store->applyDelta(delta, false);
    entry->changed = entry->changed || !delta.empty();
    if (entry->region) {
        if (FoliageSystem* foliage = worldTreeScene(project, sceneId)->findSystem<FoliageSystem>().get()) foliage->treesChanged(entity, coord.key(), delta);
    }
    return true;
}

bool editor::TerrainWorldTreeEdit::touched() const {
    for (const auto& [key, entry] : tiles) {
        if (entry.changed) return true;
    }
    return false;
}

bool editor::TerrainWorldTreeEdit::commit(const std::string& name, std::string* error, std::function<void(bool undone)> alongside) {
    if (!world) {
        if (error) *error = problem;
        return false;
    }
    std::vector<TerrainWorldTreesCmd::TileFiles> files;
    for (const auto& [key, entry] : tiles) {
        if (!entry.changed) continue;
        std::optional<std::string> after;
        // a tile that had no file and ends with no tree keeps having none
        if (!entry.store->empty() || entry.before) {
            std::vector<uint8_t> bytes;
            TreeDataFile::encode(*entry.store, bytes);
            after = std::string(bytes.begin(), bytes.end());
        }
        if (after == entry.before) continue;
        files.push_back(TerrainWorldTreesCmd::TileFiles{entry.coord, entry.before, std::move(after)});
    }
    if (files.empty()) {
        tiles.clear();
        return true;
    }

    std::string why;
    size_t written = 0;
    for (; written < files.size(); written++) {
        if (!putWorldTreeFile(*world, folder, files[written].tile, files[written].after, why)) break;
    }
    if (written < files.size()) {
        std::string ignored;
        for (size_t i = 0; i < written; i++) putWorldTreeFile(*world, folder, files[i].tile, files[i].before, ignored);
        revert();
        if (error) *error = "the trees could not be saved: " + why + ". Nothing was changed.";
        return false;
    }
    // a tile that came into memory during the edit still draws its old file
    Scene* scene = worldTreeScene(project, sceneId);
    if (FoliageSystem* foliage = scene ? scene->findSystem<FoliageSystem>().get() : nullptr) {
        for (const TerrainWorldTreesCmd::TileFiles& file : files) {
            const Tile& entry = tiles[file.tile.key()];
            if (foliage->hasRegion(entity, file.tile.key()) && foliage->getRegionTreeStore(entity, file.tile.key()) != entry.store) {
                foliage->treeStoreReplaced(entity, file.tile.key(), std::make_shared<TreeStore>(*entry.store));
            }
        }
    }
    tiles.clear();
    auto* command = new TerrainWorldTreesCmd(project, sceneId, entity, name, std::move(files), true);
    command->alongside = std::move(alongside);
    CommandHandle::get(sceneId)->addCommand(command);
    return true;
}

void editor::TerrainWorldTreeEdit::revert() {
    Scene* scene = worldTreeScene(project, sceneId);
    for (auto& [key, entry] : tiles) {
        if (entry.changed) handWorldTreesToRegion(scene, entity, entry.coord, worldTreeStoreOf(entry.before));
    }
    tiles.clear();
}

// ------------------------------------------------------------------ TerrainWorldTreesCmd

editor::TerrainWorldTreesCmd::TerrainWorldTreesCmd(Project* project, uint32_t sceneId, Entity world, std::string name, std::vector<TileFiles> tiles, bool done)
    : project(project), sceneId(sceneId), entity(world), name(std::move(name)), tiles(std::move(tiles)), done(done) {}

bool editor::TerrainWorldTreesCmd::put(bool after, std::string& error) {
    fs::path root;
    TerrainWorldRuntime* runtime = worldTreeRuntime(project, sceneId, entity, root, error);
    if (!runtime) return false;
    folder = FileUtils::pathToUtf8(root);
    Scene* scene = worldTreeScene(project, sceneId);
    bool ok = true;
    for (const TileFiles& file : tiles) {
        const std::optional<std::string>& bytes = after ? file.after : file.before;
        std::string why;
        if (!putWorldTreeFile(*runtime, root, file.tile, bytes, why)) {
            ok = false;
            error = why;
            Out::error("Terrain World: %s", why.c_str());
            continue;
        }
        handWorldTreesToRegion(scene, entity, file.tile, worldTreeStoreOf(bytes));
    }
    if (alongside) alongside(!after);
    return ok;
}

bool editor::TerrainWorldTreesCmd::execute() {
    lastError.clear();
    if (done) {
        // the edit wrote the files and changed the stores: only what goes with it runs now
        done = false;
        fs::path root;
        if (worldTreeRuntime(project, sceneId, entity, root, lastError)) folder = FileUtils::pathToUtf8(root);
        if (alongside) alongside(false);
        return true;
    }
    return put(true, lastError);
}

void editor::TerrainWorldTreesCmd::undo() {
    std::string error;
    if (!put(false, error)) undoProblem = error;
}

std::vector<editor::Command::Touched> editor::TerrainWorldTreesCmd::touchedPaths(bool) const {
    std::vector<Touched> touched;
    if (folder.empty()) return touched;
    const fs::path root = FileUtils::pathFromUtf8(folder);
    for (const TileFiles& file : tiles) touched.push_back(Touched{root / FileUtils::pathFromUtf8(terrainWorldTreeName(file.tile)), true});
    return touched;
}

size_t editor::TerrainWorldTreesCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + name.capacity() + tiles.capacity() * sizeof(TileFiles);
    for (const TileFiles& file : tiles) bytes += (file.before ? file.before->capacity() : 0) + (file.after ? file.after->capacity() : 0);
    return bytes;
}
