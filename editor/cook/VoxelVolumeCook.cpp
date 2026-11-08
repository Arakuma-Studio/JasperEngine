// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelVolumeCook.h"

#include "DocumentFile.h"
#include "component/VoxelVolumeComponent.h"
#include "io/AtomicFile.h"
#include "players/ModuleUsage.h"
#include "registry/EntityRegistry.h"
#include "subsystem/VoxelSystem.h"
#include "texture/TextureEncoders.h"
#include "voxel/VoxelFile.h"

#include <memory>
#include <string_view>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // R78: fixed, so the same content cooks to the same bytes; as the texture cook's supercompression
    constexpr int kZstdLevel = 9;

    bool compressBlock(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out) {
        return zstdCompress(std::span<const uint8_t>(bytes, size), out, kZstdLevel, nullptr);
    }

}

bool cook::isVoxelVolumeFile(const fs::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".voxelvolume";
}

bool cook::cookVoxelVolume(std::span<const uint8_t> source, const std::string& name, const VoxelCookOptions& options,
                           std::vector<uint8_t>& out, std::string& error) {
    // no budget here: what binds a file's size is the target's, at its own load
    const voxel::VolumeFileResult info = voxel::readVolumeFileInfo(source);
    if (!info.ok) {
        error = "Voxel Volume \"" + name + "\" could not be read (" + voxel::fileErrorCode(info.error) + "): " + info.message;
        return false;
    }
    if (options.hasTarget) {
        error = voxelDimensionProblem(name, info.info.desc().dimensions, options.target, options.settings);
        if (!error.empty()) return false;
    }

    // its own asset, out of VoxelAssets' cache: an open scene's volume of this path is not touched
    VoxelAsset asset;
    asset.path = name;
    voxel::generatorFromJson(info.info.generatorJson, asset.generator);
    asset.slotCount = info.info.slotCount;
    asset.terrainCut = info.info.terrainCut;
    asset.store = std::make_shared<voxel::Store>(info.info.desc());
    // also with the generator kept: a block edited back to what the base gives is what such a file
    // leaves out, and only the real base can tell (R76)
    if (asset.generator.kind == "terrain") {
        if (!options.terrainBase) {
            error = "Voxel Volume \"" + name + "\" is made over a Terrain, and no scene in the build attaches it to one: attach it "
                    "(Voxel Volume > Terrain), or bake its base (generate_voxels).";
            return false;
        }
        asset.store->setBase(options.terrainBase);
        asset.baseResolved = true;
    } else {
        VoxelAssets::resolveBase(asset);
    }
    if (!asset.baseResolved) {
        error = "Voxel Volume \"" + name + "\": its generator (" + asset.generator.kind + ") cannot be evaluated here, so it cannot be cooked. "
                "Check that its source exists.";
        return false;
    }
    const voxel::VolumeFileResult loaded = voxel::loadVolumeFile(source, *asset.store);
    if (!loaded.ok) {
        error = "Voxel Volume \"" + name + "\" could not be read (" + voxel::fileErrorCode(loaded.error) + "): " + loaded.message;
        return false;
    }

    voxel::SaveOptions save;
    save.mode = voxel::SaveOptions::Mode::Cooked;
    save.slotCount = asset.slotCount;
    save.generatorJson = options.keepGenerator ? info.info.generatorJson : std::string();
    save.terrainCut = asset.terrainCut;
    save.compress = &compressBlock;
    save.keepGenerator = options.keepGenerator;
    out = voxel::saveVolumeFile(*asset.store, save);
    return true;
}

bool cook::cookVoxelVolumeFile(const fs::path& source, const fs::path& destination, const std::string& name,
                               const VoxelCookOptions& options, std::string& error) {
    std::string text;
    if (!DocumentFiles::readBytes(source, text)) {
        error = "Voxel Volume \"" + name + "\" could not be read.";
        return false;
    }
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    std::vector<uint8_t> cooked;
    if (!cookVoxelVolume(bytes, name, options, cooked, error)) return false;
    io::AtomicWriteOptions write;
    write.skipIfUnchanged = true;       // a re-export keeps the build cache's file time
    write.durability = io::Durability::Cache;
    const io::WriteResult written = io::writeFileAtomic(destination, std::string_view(reinterpret_cast<const char*>(cooked.data()), cooked.size()), write);
    if (!written.ok()) {
        error = "Voxel Volume \"" + name + "\" could not be written: " + io::describeWriteError(written);
        return false;
    }
    return true;
}

std::set<std::string> cook::voxelVolumesGeneratedAtRuntime(const ModuleScan& scan) {
    std::set<std::string> volumes;
    for (const ModuleContent& content : scan.contents) {
        if (!content.registry) continue;
        for (Entity entity : content.entities) {
            if (!content.registry->isEntityCreated(entity)) continue;
            const VoxelVolumeComponent* volume = content.registry->findComponent<VoxelVolumeComponent>(entity);
            if (volume && volume->runtimeGeneration && !volume->volumeData.path.empty()) volumes.insert(volume->volumeData.path);
        }
    }
    return volumes;
}

std::map<std::string, std::shared_ptr<const voxel::BaseSource>> cook::voxelTerrainBases(const ModuleScan& scan, const fs::path& assetsDir,
                                                                                         std::vector<std::string>& problems) {
    std::map<std::string, std::shared_ptr<const voxel::BaseSource>> bases;
    std::map<std::string, std::string> attachedIn;      // the volume, where it was first attached
    for (const ModuleContent& content : scan.contents) {
        if (!content.registry) continue;
        for (Entity entity : content.entities) {
            if (!content.registry->isEntityCreated(entity)) continue;
            const VoxelVolumeComponent* volume = content.registry->findComponent<VoxelVolumeComponent>(entity);
            if (!volume || volume->terrain == NULL_ENTITY || volume->volumeData.path.empty()) continue;
            const std::string& path = volume->volumeData.path;
            const std::string where = content.label + ", entity " + std::to_string(entity);
            if (attachedIn.count(path)) {
                problems.push_back("Voxel Volume \"" + path + "\" is attached to a Terrain in " + attachedIn[path] + " and in " + where +
                                   ": an export cooks one ground under it. Give each its own volume.");
                continue;
            }
            attachedIn[path] = where;
            std::string text;
            if (!DocumentFiles::readBytes(assetsDir / path, text)) continue;   // the cook names an unreadable file
            const voxel::VolumeFileResult info = voxel::readVolumeFileInfo(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
            if (!info.ok) continue;
            voxel::GeneratorDesc generator;
            voxel::generatorFromJson(info.info.generatorJson, generator);
            if (generator.kind != "terrain") continue;
            std::shared_ptr<const voxel::BaseSource> base = makeAttachedTerrainBase(content.registry, entity, volume->terrain, info.info.desc().voxelSize, true);
            if (!base) {
                problems.push_back("Voxel Volume \"" + path + "\" in " + where + ": its Terrain has no height map to cook it over.");
                continue;
            }
            bases[path] = std::move(base);
        }
    }
    return bases;
}
