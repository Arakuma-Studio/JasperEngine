// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelAssetFile.h"

#include "DocumentFile.h"
#include "Out.h"
#include "Project.h"
#include "SceneBackup.h"
#include "command/CommandHandle.h"
#include "command/type/VoxelEditCmd.h"
#include "component/VoxelVolumeComponent.h"
#include "io/AtomicFile.h"
#include "io/FileData.h"
#include "io/watch/FileWatchService.h"
#include "util/FileUtils.h"
#include "voxel/VoxelFile.h"

#include <span>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr const char* kMarker = "# voxel-volume-backup: ";

    std::vector<uint8_t> encodeEditorFile(const VoxelAsset& asset) {
        voxel::SaveOptions options;
        options.mode = voxel::SaveOptions::Mode::Editor;
        options.slotCount = asset.slotCount;
        options.generatorJson = voxel::generatorToJson(asset.generator);
        options.terrainCut = asset.terrainCut;
        return voxel::saveVolumeFile(*asset.store, options);
    }

    bool refused(const VoxelAsset& asset, std::string* error) {
        if (asset.error == voxel::FileError::None && asset.store) return false;
        if (error) {
            *error = "'" + asset.path + "' did not load (" + std::string(voxel::fileErrorCode(asset.error)) + (asset.errorMessage.empty() ? "" : ": ") +
                     asset.errorMessage + "), so the file stays as it is. Fix or replace it, then open it again.";
        }
        return true;
    }

    bool writeTo(VoxelAsset& asset, const fs::path& file, std::string* error) {
        if (refused(asset, error)) return false;
        const std::vector<uint8_t> bytes = encodeEditorFile(asset);
        // the watcher's report of this write is the write arriving, not somebody's change (S05 R10)
        ScopedExpectedWrite expected(file);
        io::AtomicWriteOptions options;
        options.skipIfUnchanged = true;
        const io::WriteResult written = io::writeFileAtomic(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
        if (!written.ok()) {
            if (error) *error = io::describeWriteError(written);
            return false;
        }
        asset.fileHash = voxel::volumeFileHash(std::span<const uint8_t>(bytes));
        asset.dirty = false;
        return true;
    }

    fs::path fileOf(const VoxelAsset& asset) {
        return FileUtils::pathFromUtf8(FileData::getSystemPath(asset.path));
    }

    // what a project backup calls the volume: relative to the project
    fs::path projectRelative(Project& project, const VoxelAsset& asset) {
        const fs::path file = (project.getAssetsPath() / FileUtils::pathFromUtf8(asset.path)).lexically_normal();
        return file.lexically_relative(project.getProjectPath().lexically_normal());
    }

    template<typename Visit>
    void eachVolume(SceneProject& sceneProject, const Visit& visit) {
        Scene* scene = sceneProject.scene;
        if (!scene) return;
        std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
        if (!system) return;
        auto array = scene->getComponentArray<VoxelVolumeComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            visit(*system, array->getEntity(i), array->getComponentFromIndex(i));
        }
    }

    std::vector<fs::path> namedIn(const std::string& sceneBackupText) {
        std::vector<fs::path> out;
        std::istringstream lines(sceneBackupText);
        const std::string_view marker(kMarker);
        for (std::string line; std::getline(lines, line);) {
            if (line.compare(0, marker.size(), marker) == 0) out.push_back(FileUtils::pathFromUtf8(line.substr(marker.size())));
        }
        return out;
    }

}

bool editor::saveVoxelAsset(VoxelAsset& asset, std::string* error) {
    return writeTo(asset, fileOf(asset), error);
}

bool editor::saveNewVoxelAsset(Project& project, VoxelAsset& asset, AssetRef& ref, std::string* error) {
    const fs::path file = (project.getAssetsPath() / FileUtils::pathFromUtf8(asset.path)).lexically_normal();
    if (!writeTo(asset, file, error)) return false;
    AssetDatabase& database = project.getAssetDatabase();
    Guid guid = database.getAssetGuid(file);
    if (guid.isNull()) {
        project.onAssetFilesChanged(database.importAssets(file));
        guid = database.getAssetGuid(file);
    }
    ref.set(asset.path, guid);
    return true;
}

void editor::saveSceneVoxelAssets(Project& project, SceneProject& sceneProject) {
    eachVolume(sceneProject, [&](VoxelSystem& system, Entity entity, VoxelVolumeComponent&) {
        const std::shared_ptr<VoxelAsset> asset = system.getAsset(entity);
        if (!asset || !asset->dirty) return;
        std::string error;
        const fs::path file = (project.getAssetsPath() / FileUtils::pathFromUtf8(asset->path)).lexically_normal();
        if (!writeTo(*asset, file, &error)) {
            Out::error("Voxel Volume '%s' not saved: %s", sceneProject.scene->getEntityName(entity).c_str(), error.c_str());
            return;
        }
        // nothing left for its Auto Save copy to protect
        SceneBackups::discard(project.getProjectPath(), BackupKind::Voxel, projectRelative(project, *asset));
    });
}

std::vector<VoxelBackup> editor::collectVoxelBackups(Project& project, SceneProject& sceneProject, std::map<std::string, uint64_t>& versions) {
    std::vector<VoxelBackup> backups;
    eachVolume(sceneProject, [&](VoxelSystem& system, Entity entity, VoxelVolumeComponent&) {
        const std::shared_ptr<VoxelAsset> asset = system.getAsset(entity);
        if (!asset || !asset->dirty || !asset->store || asset->error != voxel::FileError::None) return;
        VoxelBackup backup;
        backup.source = projectRelative(project, *asset);
        for (const VoxelBackup& known : backups) {
            if (known.source == backup.source) return;   // two GameObjects, one volume
        }
        const uint64_t version = asset->store->version();
        const auto seen = versions.find(asset->path);
        if (seen == versions.end() || seen->second != version) {
            const std::vector<uint8_t> bytes = encodeEditorFile(*asset);
            backup.bytes.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            backup.baseSha1 = DocumentFiles::fileSha1(project.getProjectPath() / backup.source);
            versions[asset->path] = version;
        }
        backups.push_back(std::move(backup));
    });
    return backups;
}

void editor::writeVoxelBackups(const fs::path& projectPath, const std::vector<VoxelBackup>& backups) {
    for (const VoxelBackup& backup : backups) {
        if (backup.bytes.empty()) continue;
        SceneBackups::write(projectPath, BackupKind::Voxel, backup.source, 0, backup.bytes, backup.baseSha1, voxel::VOXEL_FILE_VERSION);
    }
}

std::string editor::voxelBackupMarkers(const std::vector<VoxelBackup>& backups) {
    std::string text;
    for (const VoxelBackup& backup : backups) {
        text += kMarker + backup.source.generic_string() + "\n";
    }
    return text;
}

std::vector<std::shared_ptr<VoxelAsset>> editor::restoreVoxelBackups(Project& project, const std::string& sceneBackupText) {
    std::vector<std::shared_ptr<VoxelAsset>> restored;
    for (const fs::path& source : namedIn(sceneBackupText)) {
        const SceneBackupInfo info = SceneBackups::scan(project.getProjectPath(), source, BackupKind::Voxel);
        std::string text;
        if (info.status != BackupStatus::Usable || !SceneBackups::readBackup(info, text)) continue;
        auto bytes = std::make_shared<std::vector<uint8_t>>(text.begin(), text.end());
        const std::string assetPath = project.normalizeToAssetsRelative(project.getProjectPath() / source).generic_string();
        std::shared_ptr<VoxelAsset> asset = VoxelAssets::fromBytes(assetPath, bytes);
        if (asset->error != voxel::FileError::None) {
            // a copy that does not read is no reason to lose the file: the file's content is kept
            Out::warning("The auto-saved copy of Voxel Volume '%s' could not be read: %s", assetPath.c_str(), asset->errorMessage.c_str());
            VoxelAssets::forget(assetPath);
            continue;
        }
        asset->dirty = true;
        Out::info("Restored Voxel Volume '%s' from its auto-saved copy (%s); save to keep it.", assetPath.c_str(), info.savedAt.c_str());
        restored.push_back(std::move(asset));
    }
    return restored;
}

void editor::discardVoxelBackups(const fs::path& projectPath, const std::string& sceneBackupText) {
    for (const fs::path& source : namedIn(sceneBackupText)) {
        SceneBackups::discard(projectPath, BackupKind::Voxel, source);
    }
}

bool editor::reloadVoxelVolumeFile(Project& project, const std::string& assetPath) {
    bool shown = false;
    bool lostEdits = false;
    size_t dropped = 0;
    std::shared_ptr<VoxelAsset> previous;
    // after this, the next acquire reads the file; the old asset lives until its steps are gone
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
        if (sceneProject->playState != ScenePlayState::STOPPED) continue;
        std::vector<Entity> owners;
        eachVolume(*sceneProject, [&](VoxelSystem& system, Entity entity, VoxelVolumeComponent& component) {
            if (component.volumeData.path != assetPath) return;
            owners.push_back(entity);
            if (const std::shared_ptr<VoxelAsset> asset = system.getAsset(entity)) {
                previous = asset;
                lostEdits = lostEdits || asset->dirty;
            }
        });
        if (owners.empty()) continue;
        if (!shown) VoxelAssets::forget(assetPath);
        shown = true;
        std::shared_ptr<VoxelSystem> system = sceneProject->scene->findSystem<VoxelSystem>();
        for (Entity owner : owners) system->reloadVolume(owner);
        sceneProject->needUpdateRender = true;
        if (CommandHistory* history = CommandHandle::find(sceneProject->id)) {
            dropped += history->dropSteps([&previous](Command& command) {
                VoxelEditCmd* edit = dynamic_cast<VoxelEditCmd*>(&command);
                return edit && edit->asset() == previous;
            });
        }
    }
    if (!shown) {
        VoxelAssets::forget(assetPath);
        return false;
    }
    if (lostEdits) {
        Out::warning("Voxel Volume '%s' changed on disk and was read again: its unsaved edits are gone.", assetPath.c_str());
    }
    if (dropped > 0) {
        Out::info("Voxel Volume '%s' changed on disk: %zu undo step%s of the old content dropped.", assetPath.c_str(), dropped, dropped == 1 ? "" : "s");
    }
    return true;
}
