// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapUtils.h"
#include "component/TerrainComponent.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "Out.h"
#include "pool/TextureDataPool.h"
#include "terrain/TerrainMapPersistence.h"
#include "terrain/TerrainSurface.h"
#include "util/TerrainMapFileWriter.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    std::atomic<uint64_t> editTextureCounter{1};
}

Texture* editor::TerrainMapUtils::findTexture(TerrainComponent& terrain, const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::HeightMap){
        return &terrain.heightMap;
    }
    if (ref.target == TerrainMapTarget::HoleMap){
        return &terrain.holeMap;
    }
    if (ref.target == TerrainMapTarget::BlendMap){
        if (ref.layer < 0 || ref.layer >= static_cast<int>(terrain.blendMaps.size())){
            return NULL;
        }
        return &terrain.blendMaps[ref.layer];
    }
    if (ref.layer < 0 || ref.layer >= static_cast<int>(terrain.foliageLayers.size())){
        return nullptr;
    }
    return &terrain.foliageLayers[ref.layer].densityMap;
}

std::string editor::TerrainMapUtils::getPropertyName(const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::HeightMap){
        return "heightMap";
    }
    if (ref.target == TerrainMapTarget::HoleMap){
        return "holeMap";
    }
    if (ref.target == TerrainMapTarget::BlendMap){
        return "blendMaps[" + std::to_string(ref.layer) + "]";
    }
    return "foliageLayers[" + std::to_string(ref.layer) + "].densityMap";
}

std::string editor::TerrainMapUtils::getContainerPropertyName(const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::BlendMap){
        return "blendMaps";
    }
    if (ref.target == TerrainMapTarget::DensityMap){
        return "foliageLayers";
    }
    return std::string();
}

bool editor::TerrainMapUtils::hasLoadedData(Texture& texture){
    if (texture.empty() || texture.isFramebuffer()){
        return false;
    }

    texture.setReleaseDataAfterLoad(false);
    TextureLoadResult result = texture.load();
    return result.state == ResourceLoadState::Finished && result.data && texture.getData().getData();
}

bool editor::TerrainMapUtils::writeFile(Project* project, const std::string& relativePath, int width, int height, int channels, int bytesPerChannel, const std::vector<unsigned char>& pixels){
    const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
    if (!project || project->getProjectPath().empty() || relativePath.empty() || width <= 0 || height <= 0 || channels <= 0 || pixels.size() < expectedSize){
        return false;
    }

    fs::path outputPath = project->resolveAssetPath(relativePath);

    std::error_code ec;
    // (in a write scope, an MCP sculpt or its undo: beneath its root)
    FileUtils::createDirectories(outputPath.parent_path(), ec);
    if (ec){
        Out::warning("Failed to create terrain texture directory: %s", outputPath.parent_path().string().c_str());
        return false;
    }

    // Encode + write on the background worker: full-map PNG writes (especially
    // 16-bit heightmaps) are slow enough to hitch the editor on every stroke end.
    // The worker has no write scope of its own: the root of this thread's scope (an MCP call,
    // an MCP-made step) goes with the job, and the worker writes beneath it.
    TerrainMapFileWriter::get().enqueue(outputPath, width, height, channels, bytesPerChannel,
        std::vector<unsigned char>(pixels.begin(), pixels.begin() + expectedSize), safewrite::scopeRootFor(outputPath));
    return true;
}

void editor::TerrainMapUtils::refresh(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref){
    if (!sceneProject){
        return;
    }
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!terrain){
        return;
    }

    if (ref.target == TerrainMapTarget::HeightMap){
        terrain->heightMapLoaded = false;
        terrain->needUpdateTerrain = true;
        terrain->needUpdateTexture = true;
        // Instances sit on the surface, so a sculpt moves them.
        terrain->needUpdateFoliage = true;
    }else if (ref.target == TerrainMapTarget::BlendMap || ref.target == TerrainMapTarget::HoleMap){
        terrain->needUpdateTexture = true;
    }else{
        terrain->needUpdateFoliage = true;
    }

    if (Texture* texture = findTexture(*terrain, ref)){
        texture->invalidateRender();
    }
}

void editor::TerrainMapUtils::refreshSharing(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref){
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* terrain = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    Texture* texture = terrain ? findTexture(*terrain, ref) : nullptr;
    if (!texture || texture->getPath(0).empty()){
        return;
    }
    const std::string path = texture->getPath(0);
    for (SceneProject& other : project->getScenes()){
        if (!other.scene){
            continue;
        }
        auto terrains = other.scene->getComponentArray<TerrainComponent>();
        for (size_t i = 0; i < terrains->size(); i++){
            const Entity otherEntity = terrains->getEntity(i);
            if (other.id == sceneId && otherEntity == entity){
                continue;
            }
            Texture* otherTexture = findTexture(terrains->getComponentFromIndex(i), ref);
            if (otherTexture && otherTexture->getPath(0) == path){
                *otherTexture = *texture;
                refresh(&other, otherEntity, ref);
                other.needUpdateRender = true;
            }
        }
    }
}

terrain::MapId editor::TerrainMapUtils::mapOfRef(const TerrainMapRef& ref){
    switch (ref.target){
        case TerrainMapTarget::HeightMap: return terrain::MapId{terrain::MapKind::Height, 0};
        case TerrainMapTarget::BlendMap: return terrain::MapId{terrain::MapKind::Blend, ref.layer};
        case TerrainMapTarget::DensityMap: return terrain::MapId{terrain::MapKind::Density, ref.layer};
        case TerrainMapTarget::HoleMap: return terrain::MapId{terrain::MapKind::Hole, 0};
    }
    return terrain::MapId{};
}

editor::TerrainMapRef editor::TerrainMapUtils::refOfMap(terrain::MapId map){
    switch (map.kind){
        case terrain::MapKind::Height: return TerrainMapRef(TerrainMapTarget::HeightMap);
        case terrain::MapKind::Blend: return TerrainMapRef(TerrainMapTarget::BlendMap, map.index);
        case terrain::MapKind::Density: return TerrainMapRef(TerrainMapTarget::DensityMap, map.index);
        case terrain::MapKind::Hole: return TerrainMapRef(TerrainMapTarget::HoleMap);
    }
    return TerrainMapRef();
}

bool editor::TerrainMapUtils::isScalarTarget(TerrainMapTarget target){
    // Height, density and holes share the single-channel storage
    return target != TerrainMapTarget::BlendMap;
}

bool editor::TerrainMapUtils::loadTextureDataFromPath(Project* project, const std::string& path, TextureData& data){
    if (path.empty()){
        return false;
    }

    fs::path texturePath = project ? project->resolveAssetPath(path) : fs::path(path);

    // An edit may be waiting for its debounce, or its write in flight: the disk read below must
    // observe it (T1 R30).
    TerrainMapPersistence::get().flushPath(texturePath.string());

    if (!data.loadTextureFromFile(texturePath.string().c_str())){
        return false;
    }

    data.setDataOwned(true);
    return data.getData() && data.getWidth() > 0 && data.getHeight() > 0 && data.getChannels() > 0;
}

// Map file stem. The layer index is left out on purpose: removing a layer shifts the ones above
// it, and a stem keyed on the index would un-own their files.
static std::string makeMapSuffix(const TerrainMapRef& ref){
    switch (ref.target){
        case TerrainMapTarget::HeightMap: return "height";
        case TerrainMapTarget::BlendMap: return "blend";
        case TerrainMapTarget::DensityMap: return "density";
        case TerrainMapTarget::HoleMap: return "hole";
    }
    return "map";
}

std::string editor::TerrainMapUtils::makeEditableTextureId(uint32_t sceneId, Entity entity, const TerrainMapRef& ref){
    return "__terrain_edit_" + std::to_string(sceneId) + "_" + std::to_string(entity) + "_" + makeMapSuffix(ref) + "_" + std::to_string(editTextureCounter.fetch_add(1));
}

std::string editor::TerrainMapUtils::makeEditableTexturePath(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref){
    fs::path baseDir = project ? project->getTerrainMapsDir() : fs::path("terrain_maps");

    const std::string suffix = makeMapSuffix(ref);
    for (int attempt = 0; attempt < 10000; attempt++){
        const uint64_t serial = editTextureCounter.fetch_add(1);
        fs::path candidatePath = baseDir / ("terrain_edit_" + std::to_string(sceneId) + "_" + std::to_string(entity) + "_" + suffix + "_" + std::to_string(serial) + ".png");
        if (!project || project->getProjectPath().empty()){
            return candidatePath.generic_string();
        }

        std::error_code ec;
        if (!fs::exists(candidatePath, ec)){
            std::error_code ec2;
            fs::path relPath = fs::relative(candidatePath, project->getAssetsPath(), ec2);
            return (!ec2 && !relPath.empty()) ? relPath.generic_string() : candidatePath.generic_string();
        }
    }

    fs::path fallbackPath = baseDir / ("terrain_edit_" + std::to_string(sceneId) + "_" + std::to_string(entity) + "_" + suffix + ".png");
    if (project && !project->getProjectPath().empty()){
        std::error_code ec;
        fs::path relPath = fs::relative(fallbackPath, project->getAssetsPath(), ec);
        if (!ec && !relPath.empty()) return relPath.generic_string();
    }
    return fallbackPath.generic_string();
}

bool editor::TerrainMapUtils::isEditableTexturePath(const std::string& path){
    if (path.empty()){
        return false;
    }

    fs::path texturePath(path);
    const std::string parentName = texturePath.parent_path().filename().string();
    const std::string filename = texturePath.filename().string();
    return parentName == "terrain_maps" && filename.rfind("terrain_edit_", 0) == 0;
}

bool editor::TerrainMapUtils::isOwnedEditableTexturePath(const std::string& path, uint32_t sceneId, Entity entity, const TerrainMapRef& ref){
    if (!isEditableTexturePath(path)){
        return false;
    }

    fs::path texturePath(path);
    const std::string suffix = makeMapSuffix(ref);
    const std::string expectedStem = "terrain_edit_" + std::to_string(sceneId) + "_" + std::to_string(entity) + "_" + suffix;
    const std::string stem = texturePath.stem().string();
    return stem == expectedStem || stem.rfind(expectedStem + "_", 0) == 0;
}

int editor::TerrainMapUtils::expectedChannels(TerrainMapTarget target){
    return isScalarTarget(target) ? 1 : 4;
}

ColorFormat editor::TerrainMapUtils::expectedFormat(TerrainMapTarget target){
    // Heightmaps use 16-bit single channel (RED16) so large maxHeight values don't quantize into
    // visible terraces; blend maps stay 8-bit RGBA and density maps 8-bit single channel.
    if (target == TerrainMapTarget::HeightMap){
        return ColorFormat::RED16;
    }
    return target == TerrainMapTarget::BlendMap ? ColorFormat::RGBA : ColorFormat::RED;
}

// Encode a normalized [0,1] height into a raw pixel buffer (single channel), 8- or 16-bit.
void editor::TerrainMapUtils::encodeHeightTexel(unsigned char* pixels, size_t texelIndex, int bytesPerChannel, float value){
    const float clamped = std::max(0.0f, std::min(1.0f, value));
    if (bytesPerChannel >= 2){
        const unsigned int quantized = static_cast<unsigned int>(std::lround(clamped * 65535.0f));
        const size_t byteIndex = texelIndex * 2;
        pixels[byteIndex] = static_cast<unsigned char>(quantized & 0xFF);
        pixels[byteIndex + 1] = static_cast<unsigned char>((quantized >> 8) & 0xFF);
    }else{
        pixels[texelIndex] = static_cast<unsigned char>(std::lround(clamped * 255.0f));
    }
}

bool editor::TerrainMapUtils::setFileBackedTextureData(Project* project, Texture& texture, const std::string& relativePath, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels){
    // What is still waiting for this file is older than what comes now, and a waiting map holds the
    // pool's entry, which the seed below has to replace
    if (project && !relativePath.empty()){
        TerrainMapPersistence::get().flushPath(project->resolveAssetPath(relativePath).string());
    }

    const int bytesPerChannel = TextureData::getBytesPerChannel(format);
    const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
    if (width <= 0 || height <= 0 || channels <= 0 || pixels.size() < expectedSize){
        return false;
    }

    if (!TerrainMapUtils::writeFile(project, relativePath, width, height, channels, bytesPerChannel, pixels)){
        return false;
    }

    // Set the path first so destroy() clears any prior pool entry for this texture, and paths[0]
    // is updated so the scene serializer references the file on save.
    texture.setPath(relativePath);
    texture.setReleaseDataAfterLoad(false);

    // Pre-populate TextureDataPool with an in-memory copy of the pixels we just wrote. This bypasses
    // async file loading (which would otherwise return Loading and force the caller to fall back to
    // an in-memory-only texture, dropping the file path and breaking persistence on the first edit).
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(expectedSize));
    if (!raw){
        return false;
    }
    std::memcpy(raw, pixels.data(), expectedSize);

    TextureData seed(width, height, static_cast<unsigned int>(expectedSize), format, channels, raw);
    seed.setDataOwned(false);

    std::array<TextureData, 6> arr;
    arr[0] = seed;
    auto poolEntry = TextureDataPool::get(relativePath, arr);

    bool seedAccepted = poolEntry && poolEntry->at(0).getData() == raw;
    if (!seedAccepted){
        // An existing pool entry was reused (use_count > 1 prevented eviction). Free our buffer
        // to avoid a leak; the existing entry will own its own pixels.
        std::free(raw);
    }

    TextureLoadResult result = texture.load();
    if (result.state != ResourceLoadState::Finished || !result.data || !texture.getData().getData()){
        return false;
    }

    TextureData& data = texture.getData();
    if (data.getChannels() != channels || data.getColorFormat() != format){
        return false;
    }

    if (seedAccepted){
        data.setDataOwned(true);
    }
    return true;
}

std::vector<unsigned char> editor::TerrainMapUtils::copyTexturePixels(TextureData& data){
    std::vector<unsigned char> pixels;
    if (!data.getData() || data.getSize() == 0){
        return pixels;
    }

    pixels.resize(data.getSize());
    std::memcpy(pixels.data(), data.getData(), data.getSize());
    return pixels;
}

std::vector<unsigned char> editor::TerrainMapUtils::convertTexturePixels(TextureData& data, TerrainMapTarget target){
    const int width = data.getWidth();
    const int height = data.getHeight();
    const int srcChannels = data.getChannels();
    const int srcBytesPerChannel = TextureData::getBytesPerChannel(data.getColorFormat());
    const int dstChannels = expectedChannels(target);
    const int dstBytesPerChannel = TextureData::getBytesPerChannel(expectedFormat(target));
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(dstChannels) * static_cast<size_t>(dstBytesPerChannel), 0);

    if (!data.getData() || width <= 0 || height <= 0 || srcChannels <= 0){
        return pixels;
    }

    unsigned char* src = static_cast<unsigned char*>(data.getData());
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            const size_t texelIndex = static_cast<size_t>(y) * width + x;
            if (isScalarTarget(target)){
                // Decode the source value (8- or 16-bit, R channel) and re-encode at the
                // destination precision. This upcasts legacy 8-bit heightmaps to 16-bit.
                const float normalized = terrain::heightTexelAt(src, texelIndex, srcChannels, srcBytesPerChannel);
                encodeHeightTexel(pixels.data(), texelIndex, dstBytesPerChannel, normalized);
            }else{
                const size_t srcIndex = texelIndex * srcChannels * srcBytesPerChannel;
                const size_t dstIndex = texelIndex * dstChannels;
                auto srcComponent = [&](int channel){
                    return src[srcIndex + std::min(channel, srcChannels - 1) * srcBytesPerChannel];
                };
                pixels[dstIndex + 0] = srcComponent(0);
                pixels[dstIndex + 1] = srcComponent(1);
                pixels[dstIndex + 2] = srcComponent(2);
                pixels[dstIndex + 3] = srcChannels >= 4 ? src[srcIndex + 3 * srcBytesPerChannel] : 255;
            }
        }
    }
    return pixels;
}

void editor::TerrainMapUtils::setOwnedTextureData(Texture& texture, const std::string& id, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels){
    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels) * static_cast<size_t>(TextureData::getBytesPerChannel(format));
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(size));
    if (!raw){
        return;
    }
    if (!pixels.empty()){
        std::memcpy(raw, pixels.data(), size);
    }else{
        std::memset(raw, 0, size);
    }

    TextureData data(width, height, static_cast<unsigned int>(size), format, channels, raw);
    data.setDataOwned(false);
    texture.setData(id, data);
    texture.getData().setDataOwned(true);
}

TerrainMapSnapshot editor::TerrainMapUtils::captureSnapshot(Project* project, Texture& texture, bool forcePixels){
    TerrainMapSnapshot snapshot;
    snapshot.minFilter = texture.getMinFilter();
    snapshot.magFilter = texture.getMagFilter();
    snapshot.wrapU = texture.getWrapU();
    snapshot.wrapV = texture.getWrapV();

    if (texture.empty() || texture.isFramebuffer()){
        return snapshot;
    }

    snapshot.empty = false;
    snapshot.path = texture.getPath(0);
    snapshot.id = texture.getId();

    if (!forcePixels && !snapshot.path.empty()){
        return snapshot;
    }

    if (TerrainMapUtils::hasLoadedData(texture)){
        TextureData& data = texture.getData();
        snapshot.width = data.getWidth();
        snapshot.height = data.getHeight();
        snapshot.channels = data.getChannels();
        snapshot.colorFormat = data.getColorFormat();
        snapshot.pixels = copyTexturePixels(data);
    }else if (!snapshot.path.empty()){
        TextureData fileData;
        if (loadTextureDataFromPath(project, snapshot.path, fileData)){
            snapshot.width = fileData.getWidth();
            snapshot.height = fileData.getHeight();
            snapshot.channels = fileData.getChannels();
            snapshot.colorFormat = fileData.getColorFormat();
            snapshot.pixels = copyTexturePixels(fileData);
        }
    }

    return snapshot;
}

bool editor::TerrainMapUtils::snapshotsEqual(const TerrainMapSnapshot& a, const TerrainMapSnapshot& b){
    return a.empty == b.empty &&
           a.path == b.path &&
           a.id == b.id &&
           a.minFilter == b.minFilter &&
           a.magFilter == b.magFilter &&
           a.wrapU == b.wrapU &&
           a.wrapV == b.wrapV &&
           a.colorFormat == b.colorFormat &&
           a.width == b.width &&
           a.height == b.height &&
           a.channels == b.channels &&
           a.pixels == b.pixels;
}

void editor::TerrainMapUtils::applySnapshotToTexture(Project* project, Texture& texture, const TerrainMapSnapshot& snapshot){
    // A rebinding may load the file back: an edit of it still waiting goes there first (T1 R30)
    if (project && !snapshot.path.empty()){
        TerrainMapPersistence::get().flushPath(project->resolveAssetPath(snapshot.path).string());
    }
    if (snapshot.empty){
        texture.destroy();
        texture = Texture();
        return;
    }

    if (!snapshot.pixels.empty() && snapshot.width > 0 && snapshot.height > 0 && snapshot.channels > 0){
        bool restoredFromFile = false;
        if (!snapshot.path.empty()){
            restoredFromFile = setFileBackedTextureData(project, texture, snapshot.path, snapshot.width, snapshot.height, snapshot.colorFormat, snapshot.channels, snapshot.pixels);
        }
        if (!restoredFromFile){
            std::string id = snapshot.id.empty() ? ("__terrain_edit_snapshot_" + std::to_string(editTextureCounter.fetch_add(1))) : snapshot.id;
            setOwnedTextureData(texture, id, snapshot.width, snapshot.height, snapshot.colorFormat, snapshot.channels, snapshot.pixels);
        }
    }else if (!snapshot.path.empty()){
        texture.setPath(snapshot.path);
    }else if (!snapshot.id.empty()){
        texture.destroy();
        texture.setId(snapshot.id);
    }else{
        texture.destroy();
        texture = Texture();
    }

    texture.setMinFilter(snapshot.minFilter);
    texture.setMagFilter(snapshot.magFilter);
    texture.setWrapU(snapshot.wrapU);
    texture.setWrapV(snapshot.wrapV);
    texture.setReleaseDataAfterLoad(false);
}

// Builds the initial pixel buffer for a freshly created terrain map, honoring the target's
// bit depth. Heightmaps optionally start at the middle (0.5) so they can be raised or lowered.
std::vector<unsigned char> editor::TerrainMapUtils::makeInitialMapPixels(TerrainMapTarget target, int width, int height, bool heightMapStartAtMiddle){
    const int channels = expectedChannels(target);
    const int bytesPerTexel = channels * TextureData::getBytesPerChannel(expectedFormat(target));
    const size_t texelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<unsigned char> pixels(texelCount * static_cast<size_t>(bytesPerTexel), 0);

    if (target == TerrainMapTarget::HeightMap && heightMapStartAtMiddle){
        const int bytesPerChannel = bytesPerTexel / channels;
        for (size_t i = 0; i < texelCount; i++){
            encodeHeightTexel(pixels.data(), i, bytesPerChannel, 0.5f);
        }
    }else if (target == TerrainMapTarget::BlendMap){
        // Opaque alpha, no painted channels yet. A density map starts empty and keeps the zeros.
        for (size_t i = 3; i < pixels.size(); i += 4){
            pixels[i] = 255;
        }
    }
    return pixels;
}

// Blend maps past the first only exist once a layer on them is painted
void editor::TerrainMapUtils::ensureMapSlot(TerrainComponent& terrain, const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::BlendMap && ref.layer >= static_cast<int>(terrain.blendMaps.size()) &&
        ref.layer < MAX_TERRAIN_BLENDMAPS){
        terrain.blendMaps.resize(ref.layer + 1);
    }
}

bool editor::TerrainMapUtils::needsEditableCopy(Texture& texture, uint32_t sceneId, Entity entity, const TerrainMapRef& ref){
    const std::string path = texture.getPath(0);
    if (texture.empty() || path.empty() || !hasLoadedData(texture) || !isOwnedEditableTexturePath(path, sceneId, entity, ref)){
        return true;
    }
    TextureData& data = texture.getData();
    return data.getChannels() != expectedChannels(ref.target) || data.getColorFormat() != expectedFormat(ref.target);
}

bool editor::TerrainMapUtils::ensureEditableMap(Project* project, SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, int resolution, bool heightMapStartAtMiddle){
    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(entity);
    ensureMapSlot(terrain, ref);
    Texture* texturePtr = TerrainMapUtils::findTexture(terrain, ref);
    if (!texturePtr){
        return false;
    }

    Texture& texture = *texturePtr;
    const TerrainMapTarget target = ref.target;
    const int channels = expectedChannels(target);
    const ColorFormat format = expectedFormat(target);

    if (texture.empty()){
        const int safeResolution = std::max(2, resolution);
        std::vector<unsigned char> pixels = makeInitialMapPixels(target, safeResolution, safeResolution, heightMapStartAtMiddle);
        const std::string path = makeEditableTexturePath(project, sceneProject->id, entity, ref);
        if (!setFileBackedTextureData(project, texture, path, safeResolution, safeResolution, format, channels, pixels)){
            setOwnedTextureData(texture, makeEditableTextureId(sceneProject->id, entity, ref), safeResolution, safeResolution, format, channels, pixels);
        }
        return true;
    }

    const std::string path = texture.getPath(0);
    TextureData fileData;
    TextureData* data = nullptr;
    bool loadedFromFile = false;

    if (TerrainMapUtils::hasLoadedData(texture)){
        data = &texture.getData();
    }else if (!path.empty() && loadTextureDataFromPath(project, path, fileData)){
        data = &fileData;
        loadedFromFile = true;
    }else{
        return false;
    }

    const bool needsEditableFile = path.empty() || !isOwnedEditableTexturePath(path, sceneProject->id, entity, ref);
    const bool shouldConvert = loadedFromFile || needsEditableFile || data->getChannels() != channels || data->getColorFormat() != format;
    if (shouldConvert){
        std::vector<unsigned char> pixels = convertTexturePixels(*data, target);
        const std::string editablePath = needsEditableFile ? makeEditableTexturePath(project, sceneProject->id, entity, ref) : path;
        if (!setFileBackedTextureData(project, texture, editablePath, data->getWidth(), data->getHeight(), format, channels, pixels)){
            setOwnedTextureData(texture, makeEditableTextureId(sceneProject->id, entity, ref), data->getWidth(), data->getHeight(), format, channels, pixels);
        }
    }else{
        texture.getData().setDataOwned(true);
    }

    return true;
}
