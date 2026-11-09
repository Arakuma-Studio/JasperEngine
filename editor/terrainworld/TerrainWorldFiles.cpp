// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldFiles.h"

#include "Out.h"
#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"
#include "subsystem/TerrainWorldSystem.h"
#include "util/AssetDatabase.h"

#include "yaml-cpp/yaml.h"
#include "zstd.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terrain;

namespace fs = std::filesystem;

namespace {

    bool writeAtomic(const fs::path& path, const std::string& bytes, io::Durability durability, std::string& error) {
        // S05 R10: the editor's own write, which the watcher does not report back as a change
        ScopedExpectedWrite expected(path);
        io::AtomicWriteOptions options;
        options.durability = durability;
        options.skipIfUnchanged = true;
        const io::WriteResult result = io::writeFileAtomic(path, bytes, options);
        if (!result.ok()) {
            error = result.message.empty() ? "could not write " + path.string() : result.message;
            return false;
        }
        expected.reported(path);
        return true;
    }

    bool readWorldFile(const fs::path& path, std::string& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        std::ostringstream text;
        text << file.rdbuf();
        out = text.str();
        return true;
    }

    std::string worldHex64(uint64_t value) {
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }

    bool parseTileName(const std::string& name, TileCoord& out) {
        int x = 0, z = 0;
        char tail[8] = {};
        if (std::sscanf(name.c_str(), "%d_%d.jtil%1s", &x, &z, tail) != 3 || std::string(tail) != "e") return false;
        const TileCoord coord(x, z);
        if (!coord.addressable() || terrainWorldTileName(coord) != "tiles/" + name) return false;
        out = coord;
        return true;
    }

    // the editor's runtimes of worlds that are files here, out of Play (Play never writes, R31)
    template<typename F>
    void forEachEditableWorld(F&& fn) {
        TerrainWorldSystem::forEachRuntime([&](TerrainWorldRuntime& runtime) {
            if (runtime.playing() || !runtime.started() || !runtime.source().editable()) return;
            auto* source = dynamic_cast<EditorWorldSource*>(&runtime.source());
            if (source) fn(runtime, *source);
        });
    }

    std::map<const TerrainWorldRuntime*, double>& dirtySince() {
        static std::map<const TerrainWorldRuntime*, double> since;
        return since;
    }

}

TileCodec jasperengine::editor::terrainWorldEditorCodec() {
    TileCodec codec = terrainWorldDecodeCodec();
    codec.compress = [](std::span<const uint8_t> in, int level, std::vector<uint8_t>& out) {
        out.resize(ZSTD_compressBound(in.size()));
        const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
        if (ZSTD_isError(written)) return false;
        out.resize(written);
        return true;
    };
    return codec;
}

std::vector<TileCoord> jasperengine::editor::scanWorldTiles(const fs::path& folder) {
    std::vector<TileCoord> tiles;
    std::error_code ec;
    for (fs::directory_iterator it(folder / "tiles", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        TileCoord coord;
        if (parseTileName(it->path().filename().string(), coord)) tiles.push_back(coord);
    }
    std::sort(tiles.begin(), tiles.end(), [](TileCoord a, TileCoord b) { return a.key() < b.key(); });
    return tiles;
}

bool jasperengine::editor::readWorldManifest(const fs::path& folder, WorldManifest& out, std::string& error) {
    std::string text;
    if (!readWorldFile(folder / "manifest.yaml", text)) {
        error = "the world has no manifest.yaml: " + folder.string();
        return false;
    }
    WorldManifest manifest;
    try {
        const YAML::Node root = YAML::Load(text);
        manifest.formatVersion = root["formatVersion"].as<uint32_t>(1);
        if (manifest.formatVersion > WORLD_FORMAT_VERSION) {
            // S03 R23: a newer world is not opened, so nothing overwrites what this build cannot read
            error = "manifest.yaml was made by a newer version of the editor (format " + std::to_string(manifest.formatVersion) + ")";
            return false;
        }
        manifest.guid = root["guid"].as<std::string>("");
        WorldLattice& lattice = manifest.lattice;
        lattice.tileQuads = root["tileQuads"].as<int>(512);
        lattice.spacing = root["spacing"].as<float>(1.0f);
        lattice.overviewStride = root["overviewStride"].as<int>(16);
        lattice.blendQuads = root["blendQuads"].as<int>(0);
        lattice.holeTexels = root["holeTexels"].as<int>(0);
        lattice.densityTexels = root["densityTexels"].as<int>(0);
        WorldMaps& maps = manifest.maps;
        maps.maxHeight = root["maxHeight"].as<float>(100.0f);
        maps.baseHeight = static_cast<uint16_t>(std::clamp(root["baseHeight"].as<int>(0), 0, 65535));
        maps.blendMaps = std::clamp(root["blendMaps"].as<int>(0), 0, 3);
        if (const YAML::Node fills = root["blendFill"]; fills && fills.IsSequence()) {
            for (size_t i = 0; i < fills.size() && i < 8; i++) {
                for (size_t c = 0; c < 4 && c < fills[i].size(); c++) maps.blendFill[i][c] = fills[i][c].as<float>(0.0f);
            }
        }
        maps.hasHoles = root["hasHoles"].as<bool>(false);
        maps.densityMaps = std::clamp(root["densityMaps"].as<int>(0), 0, 64);
        manifest.layerHash = std::stoull(root["layerHash"].as<std::string>("0"), nullptr, 16);
    } catch (const std::exception& exception) {
        error = std::string("manifest.yaml could not be read: ") + exception.what();
        return false;
    }
    std::string why;
    if (!validateLattice(manifest.lattice, &why)) {
        error = "manifest.yaml: " + why;
        return false;
    }
    // R48: the tiles are the files present; their min and max from each one's own pyramid
    const uint32_t only[] = {TILE_SECTION_MINMAX, TILE_SECTION_EDGE};
    const TileCodec codec = terrainWorldDecodeCodec();
    for (const TileCoord& tile : scanWorldTiles(folder)) {
        WorldTileInfo info;
        info.coord = tile;
        std::string bytes;
        WorldTile data;
        if (readWorldFile(folder / terrainWorldTileName(tile), bytes) &&
            decodeTile(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), codec, data, only) == TileError::None &&
            !data.minMax.empty()) {
            const TileMinMax whole = tileNodeMinMax(data, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
            info.minHeight = whole.min;
            info.maxHeight = whole.max;
            manifest.skirtDepth = std::max(manifest.skirtDepth, data.skirtDepth);
        }
        std::error_code ec;
        info.trees = fs::is_regular_file(folder / terrainWorldTreeName(tile), ec);
        manifest.tiles.push_back(info);
    }
    out = std::move(manifest);
    return true;
}

bool jasperengine::editor::writeWorldManifest(const fs::path& folder, const WorldManifest& manifest, std::string& error) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "formatVersion" << YAML::Value << manifest.formatVersion;
    out << YAML::Key << "guid" << YAML::Value << manifest.guid;
    out << YAML::Key << "tileQuads" << YAML::Value << manifest.lattice.tileQuads;
    out << YAML::Key << "spacing" << YAML::Value << manifest.lattice.spacing;
    out << YAML::Key << "overviewStride" << YAML::Value << manifest.lattice.overviewStride;
    out << YAML::Key << "blendQuads" << YAML::Value << manifest.lattice.blendQuadCount();
    out << YAML::Key << "holeTexels" << YAML::Value << manifest.lattice.holeSide();
    out << YAML::Key << "densityTexels" << YAML::Value << manifest.lattice.densitySide();
    out << YAML::Key << "maxHeight" << YAML::Value << manifest.maps.maxHeight;
    out << YAML::Key << "baseHeight" << YAML::Value << static_cast<int>(manifest.maps.baseHeight);
    out << YAML::Key << "blendMaps" << YAML::Value << manifest.maps.blendMaps;
    if (manifest.maps.blendMaps > 0) {
        out << YAML::Key << "blendFill" << YAML::Value << YAML::BeginSeq;
        for (int i = 0; i < manifest.maps.blendMaps; i++) {
            out << YAML::Flow << YAML::BeginSeq;
            for (float channel : manifest.maps.blendFill[i]) out << channel;
            out << YAML::EndSeq;
        }
        out << YAML::EndSeq;
    }
    out << YAML::Key << "hasHoles" << YAML::Value << manifest.maps.hasHoles;
    out << YAML::Key << "densityMaps" << YAML::Value << manifest.maps.densityMaps;
    out << YAML::Key << "layerHash" << YAML::Value << worldHex64(manifest.layerHash);
    out << YAML::EndMap;
    std::string text = out.c_str();
    text += "\n";
    return writeAtomic(folder / "manifest.yaml", text, io::Durability::Document, error);
}

bool jasperengine::editor::writeTileFile(const fs::path& folder, const WorldTile& tile, std::string& error) {
    std::vector<uint8_t> bytes;
    TileEncodeOptions options;
    options.level = 9;
    if (!encodeTile(tile, terrainWorldEditorCodec(), options, bytes, &error)) return false;
    return writeAtomic(folder / terrainWorldTileName(tile.coord), std::string(bytes.begin(), bytes.end()), io::Durability::Document, error);
}

bool jasperengine::editor::writeTileBytes(const fs::path& folder, TileCoord tile, const std::string& bytes, std::string& error) {
    return writeAtomic(folder / terrainWorldTileName(tile), bytes, io::Durability::Document, error);
}

bool jasperengine::editor::writeTreeBytes(const fs::path& folder, TileCoord tile, const std::string& bytes, std::string& error) {
    return writeAtomic(folder / terrainWorldTreeName(tile), bytes, io::Durability::Document, error);
}

bool jasperengine::editor::removeTreeFile(const fs::path& folder, TileCoord tile, std::string& error) {
    const fs::path path = folder / terrainWorldTreeName(tile);
    ScopedExpectedWrite expected(path);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        error = "could not remove " + path.string() + ": " + ec.message();
        return false;
    }
    expected.reportedRemoved(path);
    return true;
}

bool jasperengine::editor::removeTileFile(const fs::path& folder, TileCoord tile, std::string& error) {
    const fs::path path = folder / terrainWorldTileName(tile);
    ScopedExpectedWrite expected(path);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        error = "could not remove " + path.string() + ": " + ec.message();
        return false;
    }
    expected.reportedRemoved(path);
    return true;
}

bool jasperengine::editor::createWorldFolder(const fs::path& folder, WorldManifest& manifest, const std::vector<TileCoord>& tiles, std::string& error,
                                             const std::vector<float>& layerColours, int layerCount) {
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        error = "a file or folder is already at " + folder.string();
        return false;
    }
    if (manifest.guid.empty()) manifest.guid = AssetDatabase::generateGuid();
    manifest.tiles.clear();
    // the tiles first and the manifest last: a folder without a manifest is not yet a world
    for (const TileCoord& coord : tiles) {
        WorldTile tile;
        initTile(tile, manifest.lattice, coord, manifest.maps.baseHeight);
        if (!layerColours.empty()) buildOverviewColour(tile, manifest.lattice, layerColours, layerCount);
        if (!writeTileFile(folder, tile, error)) return false;
        manifest.tiles.push_back(WorldTileInfo{coord, manifest.maps.baseHeight, manifest.maps.baseHeight});
    }
    return writeWorldManifest(folder, manifest, error);
}

bool jasperengine::editor::writeWorldFolder(const fs::path& folder, WorldManifest& manifest, const std::vector<std::shared_ptr<WorldTile>>& tiles,
                                            const std::map<uint32_t, std::vector<uint8_t>>& trees, std::string& error) {
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        error = "a file or folder is already at " + folder.string();
        return false;
    }
    if (manifest.guid.empty()) manifest.guid = AssetDatabase::generateGuid();
    manifest.tiles.clear();
    // the tiles first and the manifest last: a folder without a manifest is not yet a world
    for (const std::shared_ptr<WorldTile>& tile : tiles) {
        if (!writeTileFile(folder, *tile, error)) return false;
        WorldTileInfo info;
        info.coord = tile->coord;
        const TileMinMax whole = tileNodeMinMax(*tile, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
        info.minHeight = whole.min;
        info.maxHeight = whole.max;
        const auto treeFile = trees.find(tile->coord.key());
        if (treeFile != trees.end()) {
            if (!writeAtomic(folder / terrainWorldTreeName(tile->coord), std::string(treeFile->second.begin(), treeFile->second.end()), io::Durability::Document, error)) {
                return false;
            }
            info.trees = true;
        }
        manifest.skirtDepth = std::max(manifest.skirtDepth, tile->skirtDepth);
        manifest.tiles.push_back(info);
    }
    return writeWorldManifest(folder, manifest, error);
}

fs::path jasperengine::editor::worldOverviewCachePath(const fs::path& projectInternal, const std::string& guid) {
    return projectInternal / "cache" / "terrainworld" / guid / "overview.jtile";
}

std::string jasperengine::editor::worldTileFingerprint(const fs::path& folder) {
    std::string text;
    std::error_code ec;
    for (const TileCoord& tile : scanWorldTiles(folder)) {
        const fs::path path = folder / terrainWorldTileName(tile);
        const auto size = fs::file_size(path, ec);
        const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
        text += terrainWorldTileName(tile) + ":" + std::to_string(size) + ":" + std::to_string(static_cast<long long>(time)) + "\n";
    }
    return text;
}

EditorWorldSource::EditorWorldSource(fs::path folder, fs::path projectInternal): worldFolder(std::move(folder)), internal(std::move(projectInternal)) {}

bool EditorWorldSource::manifest(WorldManifest& out, std::string& error) {
    if (!readWorldManifest(worldFolder, out, error)) return false;
    guid = out.guid;
    return true;
}

std::string EditorWorldSource::tilePath(TileCoord tile) const {
    return (worldFolder / terrainWorldTileName(tile)).string();
}

std::string EditorWorldSource::treePath(TileCoord tile) const {
    return (worldFolder / terrainWorldTreeName(tile)).string();
}

fs::path EditorWorldSource::overviewCache() const {
    return worldOverviewCachePath(internal, guid.empty() ? std::string("unknown") : guid);
}

std::string EditorWorldSource::overviewPath() const {
    // R52: the cache stands while the tiles it was made from do; else the runtime assembles it
    std::string stamp;
    const fs::path cache = overviewCache();
    fs::path fingerprint = cache;
    fingerprint.replace_extension(".fingerprint");
    if (!readWorldFile(fingerprint, stamp) || stamp != worldTileFingerprint(worldFolder)) return std::string();
    return cache.string();
}

bool TerrainWorldFiles::flushWorld(TerrainWorldRuntime& runtime, std::string* error) {
    auto* source = dynamic_cast<EditorWorldSource*>(&runtime.source());
    if (!source || runtime.playing()) return true;
    runtime.settleEdits();
    bool ok = true;
    std::string why;
    for (const TileCoord& tile : runtime.takeRemovedFiles()) {
        if (!removeTileFile(source->folder(), tile, why)) {
            ok = false;
            Out::error("Terrain World: %s", why.c_str());
        }
    }
    bool wroteTiles = false;
    for (const TileCoord& tile : runtime.takeDirtyFiles()) {
        std::shared_ptr<const WorldTile> data = runtime.resident(tile);
        if (!data) continue;
        if (writeTileFile(source->folder(), *data, why)) {
            runtime.releaseFlushed(tile);
            wroteTiles = true;
        } else {
            ok = false;
            // stays dirty and is tried again at the next flush (T1 R31)
            runtime.changed(tile);
            Out::error("Terrain World: tile (%i, %i) could not be saved: %s", tile.x, tile.z, why.c_str());
        }
    }
    if (!writeWorldManifest(source->folder(), runtime.manifest(), why)) {
        ok = false;
        Out::error("Terrain World: %s", why.c_str());
    }
    if (runtime.overviewNeedsSave() || wroteTiles) {
        std::vector<uint8_t> bytes;
        if (!runtime.getOverview().empty() &&
            encodeOverview(runtime.getOverview(), runtime.lattice(), terrainWorldEditorCodec(), 3, bytes, &why)) {
            const fs::path cache = source->overviewCache();
            fs::path fingerprint = cache;
            fingerprint.replace_extension(".fingerprint");
            if (writeAtomic(cache, std::string(bytes.begin(), bytes.end()), io::Durability::Cache, why) &&
                writeAtomic(fingerprint, worldTileFingerprint(source->folder()), io::Durability::Cache, why)) {
                runtime.overviewSaved();
            }
        }
    }
    if (!ok && error) *error = why;
    return ok;
}

bool TerrainWorldFiles::flushAll() {
    bool ok = true;
    forEachEditableWorld([&](TerrainWorldRuntime& runtime, EditorWorldSource&) {
        ok = flushWorld(runtime) && ok;
        dirtySince().erase(&runtime);
    });
    return ok;
}

void TerrainWorldFiles::update(double nowSeconds) {
    std::map<const TerrainWorldRuntime*, double> seen;
    forEachEditableWorld([&](TerrainWorldRuntime& runtime, EditorWorldSource&) {
        if (!runtime.hasDirtyFiles() && !runtime.overviewNeedsSave()) return;
        const auto it = dirtySince().find(&runtime);
        const double since = it == dirtySince().end() ? nowSeconds : it->second;
        if (nowSeconds - since >= DEBOUNCE_SECONDS) {
            flushWorld(runtime);
            return;
        }
        seen[&runtime] = since;
    });
    dirtySince() = std::move(seen);
}
