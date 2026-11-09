// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapPersistence.h"
#include "component/TerrainComponent.h"

#include "Project.h"
#include "pool/TextureDataPool.h"
#include "util/SafeWrite.h"
#include "util/TerrainMapFileWriter.h"
#if JASPERENGINE_TERRAIN_WORLD
#include "terrainworld/TerrainWorldFiles.h"
#endif

#include <chrono>
#include <utility>

using namespace jasperengine;

editor::TerrainMapPersistence& editor::TerrainMapPersistence::get(){
    static TerrainMapPersistence persistence;
    return persistence;
}

double editor::TerrainMapPersistence::clockSeconds(){
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void editor::TerrainMapPersistence::markDirty(Project* project, Texture& texture){
    markDirty(project, texture, clockSeconds());
}

void editor::TerrainMapPersistence::markDirty(Project* project, Texture& texture, double nowSeconds){
    const std::string relativePath = texture.getPath(0);
    if (!project || project->getProjectPath().empty() || relativePath.empty()){
        return;
    }
    const std::filesystem::path outputPath = project->resolveAssetPath(relativePath);
    Dirty& dirty = dirtyMaps[outputPath.string()];
    dirty.root = safewrite::scopeRootFor(outputPath);
    dirty.lastEdit = nowSeconds;
    dirty.entry = TextureDataPool::get(texture.getId());
}

void editor::TerrainMapPersistence::beginStroke(){
    strokes++;
}

void editor::TerrainMapPersistence::endStroke(){
    if (strokes > 0){
        strokes--;
    }
}

void editor::TerrainMapPersistence::update(){
    if (!dirtyMaps.empty()){
        update(clockSeconds());
    }
}

void editor::TerrainMapPersistence::update(double nowSeconds){
    if (strokes > 0){
        return;
    }
    for (auto it = dirtyMaps.begin(); it != dirtyMaps.end();){
        if (nowSeconds - it->second.lastEdit >= DEBOUNCE_SECONDS){
            handOver(it->first, it->second);
            it = dirtyMaps.erase(it);
        }else{
            ++it;
        }
    }
}

bool editor::TerrainMapPersistence::isDue() const{
    return !dirtyMaps.empty() && isDue(clockSeconds());
}

bool editor::TerrainMapPersistence::isDue(double nowSeconds) const{
    if (strokes > 0){
        return false;
    }
    for (const auto& [path, dirty] : dirtyMaps){
        if (nowSeconds - dirty.lastEdit >= DEBOUNCE_SECONDS){
            return true;
        }
    }
    return false;
}

void editor::TerrainMapPersistence::flushPath(const std::string& absolutePath){
    auto it = dirtyMaps.find(absolutePath);
    if (it != dirtyMaps.end()){
        handOver(it->first, it->second);
        dirtyMaps.erase(it);
    }
    TerrainMapFileWriter::get().flushPath(absolutePath);
}

void editor::TerrainMapPersistence::handOverAll(){
    for (const auto& [path, dirty] : dirtyMaps){
        handOver(path, dirty);
    }
    dirtyMaps.clear();
}

bool editor::TerrainMapPersistence::flushAll(){
#if JASPERENGINE_TERRAIN_WORLD
    // T4 R31: a Terrain World's edited tiles reach their files at the same points
    const bool worldsOk = TerrainWorldFiles::flushAll();
#else
    const bool worldsOk = true;
#endif
    handOverAll();
    return TerrainMapFileWriter::get().flushAll() && worldsOk;
}

size_t editor::TerrainMapPersistence::pendingCount() const{
    return dirtyMaps.size();
}

std::vector<std::string> editor::TerrainMapPersistence::failedPaths() const{
    return TerrainMapFileWriter::get().failedPaths();
}

void editor::TerrainMapPersistence::handOver(const std::string& absolutePath, const Dirty& dirty){
    if (!dirty.entry){
        return;
    }
    TextureData& data = dirty.entry->at(0);
    const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
    if (!pixels || data.getWidth() <= 0 || data.getHeight() <= 0 || data.getChannels() <= 0){
        return;
    }
    const int bytesPerChannel = TextureData::getBytesPerChannel(data.getColorFormat());
    const size_t bytes = static_cast<size_t>(data.getWidth()) * data.getHeight() * data.getChannels() * bytesPerChannel;
    TerrainMapFileWriter::get().enqueue(absolutePath, data.getWidth(), data.getHeight(), data.getChannels(), bytesPerChannel,
                                        std::vector<unsigned char>(pixels, pixels + bytes), dirty.root);
}
