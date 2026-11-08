// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "foliage/TreeDataFile.h"

#include "Log.h"
#include "Project.h"
#include "component/TerrainComponent.h"
#include "subsystem/FoliageSystem.h"
#include "io/AtomicFile.h"

#include "zstd.h"

#include <set>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::terrain;
namespace fs = std::filesystem;

namespace {

    bool treeFileCompress(const void* input, size_t inputBytes, std::vector<uint8_t>& output) {
        output.resize(ZSTD_compressBound(inputBytes));
        const size_t written = ZSTD_compress(output.data(), output.size(), input, inputBytes, 3);
        if (ZSTD_isError(written)) return false;
        output.resize(written);
        return true;
    }

    std::string treeFileStem(uint32_t sceneId, Entity terrain) {
        return "terrain_edit_trees_" + std::to_string(sceneId) + "_" + std::to_string(terrain) + "_";
    }

    // what waits for its flush, in the order it was asked
    std::set<std::pair<uint32_t, Entity>>& dirtyStores() {
        static std::set<std::pair<uint32_t, Entity>> stores;
        return stores;
    }

}

void editor::TreeDataFile::encode(const TreeStore& store, std::vector<uint8_t>& bytes) {
    store.encode(bytes, treeFileCompress);
}

bool editor::TreeDataFile::isOwnedPath(const std::string& path, uint32_t sceneId, Entity terrain) {
    if (path.empty()) return false;
    const fs::path file(path);
    return file.parent_path().filename() == "terrain_maps" && file.filename().string().rfind(treeFileStem(sceneId, terrain), 0) == 0;
}

std::string editor::TreeDataFile::makePath(Project* project, uint32_t sceneId, Entity terrain) {
    const fs::path directory = project->getTerrainMapsDir();
    for (int serial = 0;; serial++) {
        const fs::path candidate = directory / (treeFileStem(sceneId, terrain) + std::to_string(serial) + ".jetrees");
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            std::error_code relativeError;
            const fs::path relative = fs::relative(candidate, project->getAssetsPath(), relativeError);
            return (!relativeError && !relative.empty()) ? relative.generic_string() : candidate.generic_string();
        }
    }
}

void editor::TreeDataFile::markDirty(Project* project, uint32_t sceneId, Entity terrain) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return;
    // R62: a duplicate shares the file until this, its first write
    TerrainComponent* component = sceneProject->scene->findComponent<TerrainComponent>(terrain);
    if (!component) return;
    if (!isOwnedPath(component->treeData, sceneId, terrain)) {
        component->treeData = AssetRef(makePath(project, sceneId, terrain));
        // the store in memory is this path's, whatever the old file holds
        FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
        foliage->treeFileWritten(terrain, component->treeData);
    }
    dirtyStores().insert({sceneId, terrain});
    sceneProject->isModified = true;
}

bool editor::TreeDataFile::flush(Project* project, uint32_t sceneId, Entity terrain, std::string* error) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return true;
    FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
    TerrainComponent* component = sceneProject->scene->findComponent<TerrainComponent>(terrain);
    if (!component) return true;

    std::string refusal;
    if (foliage->isTreeStoreRefused(terrain, &refusal)) {
        if (error) *error = component->treeData.str() + " was refused when it loaded (" + refusal + "); it is not overwritten";
        return false;
    }
    std::shared_ptr<TreeStore> store = foliage->getTreeStore(terrain);
    if (!store) return true;

    std::vector<uint8_t> bytes;
    encode(*store, bytes);
    const fs::path target = project->resolveAssetPath(component->treeData.str());
    const io::WriteResult written = io::writeFileAtomic(target, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (written.error != io::WriteError::None) {
        if (error) *error = "Could not write " + target.string() + ": " + written.message;
        return false;
    }
    foliage->treeFileWritten(terrain, component->treeData);
    dirtyStores().erase({sceneId, terrain});
    return true;
}

bool editor::TreeDataFile::flushAll(Project* project, std::string* error) {
    bool ok = true;
    const std::set<std::pair<uint32_t, Entity>> stores = dirtyStores();
    for (const std::pair<uint32_t, Entity>& entry : stores) {
        std::string message;
        if (!flush(project, entry.first, entry.second, &message)) {
            ok = false;
            Log::error("%s", message.c_str());
            if (error && error->empty()) *error = message;
        }
    }
    return ok;
}
