// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The .voxelvolume on disk from the editor's side (docs/specs/T6-voxel-volumes.md R61, R76, R77):
// atomic writes the file watcher recognises as ours, the save with the scene, what Auto Save keeps
// of a dirty volume and how recovery brings it back.

#include "asset/AssetRef.h"
#include "subsystem/VoxelSystem.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // Writes the volume to its file in the editor form (R76: only what differs from the base). False,
    // with the reason, when it cannot; the file then holds what it held. A volume whose file did not
    // load is refused: writing it would destroy what could not be read (R61).
    bool saveVoxelAsset(VoxelAsset& asset, std::string* error);

    // The first save of a new volume, whose path is relative to the assets folder: it gets its sidecar
    // GUID now (S04), and `ref` names it.
    bool saveNewVoxelAsset(Project& project, VoxelAsset& asset, AssetRef& ref, std::string* error);

    // The scene's save: every dirty volume it shows. A failure is reported and leaves that volume
    // dirty; the scene is written all the same, so a read-only volume loses no other work.
    void saveSceneVoxelAssets(Project& project, SceneProject& sceneProject);

    // ---- Auto Save and recovery (R77, S03 R13-R19)

    struct VoxelBackup {
        std::filesystem::path source;   // project-relative
        std::string bytes;              // empty: already backed up at this version
        std::string baseSha1;
    };

    // The dirty volumes of a scene, encoded here (the store's tables belong to the main thread).
    // `versions` remembers what was last backed up, so an unchanged volume is not encoded again.
    std::vector<VoxelBackup> collectVoxelBackups(Project& project, SceneProject& sceneProject, std::map<std::string, uint64_t>& versions);
    // From any thread
    void writeVoxelBackups(const std::filesystem::path& projectPath, const std::vector<VoxelBackup>& backups);
    // What a scene backup's text ends with. A comment for YAML: the scene reads as it would, and a
    // volume edit, which leaves the scene's own text as it was, still makes a backup worth offering.
    std::string voxelBackupMarkers(const std::vector<VoxelBackup>& backups);
    // The volumes a scene backup names, as VoxelAssets holds them: restored and dirty, to be kept
    // alive by the scene until its VoxelSystem takes them
    std::vector<std::shared_ptr<VoxelAsset>> restoreVoxelBackups(Project& project, const std::string& sceneBackupText);
    // The scene backup is going: so are the copies it names
    void discardVoxelBackups(const std::filesystem::path& projectPath, const std::string& sceneBackupText);

    // ---- external change (S05, R77)

    // Another program changed the file: every open scene's volume reads it again, and the undo steps
    // of the old content go, with a note. False when nothing shows this file.
    bool reloadVoxelVolumeFile(Project& project, const std::string& assetPath);

}
