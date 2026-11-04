// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Scene autosave backups and crash recovery (docs/data-safety.md, docs/specs/S03-data-safety.md
// R13-R18). Autosave never writes the user's file: it writes a copy under
// .jasperengine/backup/ with a JSON sidecar
//   {source, base_sha1, saved_at, format_version, editor_version, pid}
// where base_sha1 is the SHA-1 of the source file as this session last loaded or saved it.
// A backup is offered again only when the file still has exactly those bytes, so a git pull
// or another editor's save makes it stale instead of silently winning (no mtime involved).
//
// Pure filesystem functions: the editor, the CLI and the tests use them alike.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    enum class BackupKind {
        Scene,      // backup/scenes/<project-relative path>
        Untitled,   // backup/untitled/<sceneId>.scene
        Prefab,     // backup/prefabs/<project-relative path> (Prefab Mode with Auto Save off)
        Voxel       // backup/voxels/<project-relative path>: a dirty volume, beside its scene's (T6 R77)
    };

    enum class BackupStatus {
        None,
        // newer than the file it was made from, which is still unchanged: offered
        Usable,
        // the file changed since: moved to backup/stale/, never offered nor deleted
        Stale
    };

    enum class RecoveryChoice {
        // open the saved file and leave the backup (offered again next time): Decide Later
        Keep,
        // load the backup's content into the scene (the document path stays)
        Restore,
        // delete the backup and open the saved file
        Discard
    };

    struct SceneBackupInfo {
        BackupKind kind = BackupKind::Scene;
        BackupStatus status = BackupStatus::None;
        // project-relative; empty for an untitled scene
        std::filesystem::path source;
        // absolute path of the backup (of where it went, for a stale one)
        std::filesystem::path backup;
        std::string baseSha1;
        // ISO 8601 local time, and seconds since the epoch
        std::string savedAt;
        int64_t savedAtEpoch = 0;
        int formatVersion = 0;
        std::string editorVersion;
        int64_t pid = 0;
        // untitled backups: the scene id and name the backup holds
        uint32_t sceneId = 0;
        // "14:32:05"
        std::string savedAtClock() const;
    };

    // What a scene load did with its backup (Project::getLastRecovery)
    struct RecoveryOutcome {
        // a recovery check ran for the last opened scene
        bool checked = false;
        SceneBackupInfo backup;
        RecoveryChoice applied = RecoveryChoice::Keep;
    };

    class SceneBackups {
    public:
        static std::filesystem::path root(const std::filesystem::path& projectPath);
        static std::filesystem::path backupPath(const std::filesystem::path& projectPath, BackupKind kind,
                const std::filesystem::path& source, uint32_t sceneId = 0);
        static std::filesystem::path sidecarPath(const std::filesystem::path& backup);

        // Writes a backup and its sidecar (Document atomic writes); false with the reason logged
        static bool write(const std::filesystem::path& projectPath, BackupKind kind, const std::filesystem::path& source,
                uint32_t sceneId, const std::string& bytes, const std::string& baseSha1, int formatVersion);

        // R17 for one scene file: Usable when its backup exists, its base_sha1 is the file's
        // SHA-1, its bytes differ from the file's and its writer is not a running process;
        // Stale (moved to backup/stale/, a warning names it) when the file changed since;
        // None otherwise (a backup identical to the file is deleted: it holds nothing)
        static SceneBackupInfo scan(const std::filesystem::path& projectPath, const std::filesystem::path& source, BackupKind kind = BackupKind::Scene);
        // Every backup under backup/scenes, backup/untitled and backup/prefabs, with its status
        // (nothing is moved: a listing). A volume's copy is not one to offer: it comes with its scene.
        static std::vector<SceneBackupInfo> list(const std::filesystem::path& projectPath);
        // The untitled backups of processes that are gone (offered once at project open)
        static std::vector<SceneBackupInfo> orphanedUntitled(const std::filesystem::path& projectPath);

        static bool readSidecar(const std::filesystem::path& backup, SceneBackupInfo& info);
        static bool readBackup(const SceneBackupInfo& info, std::string& bytes);
        // Deletes a backup and its sidecar
        static bool discard(const std::filesystem::path& backup);
        static bool discard(const std::filesystem::path& projectPath, BackupKind kind, const std::filesystem::path& source, uint32_t sceneId = 0);
        // A clean close: every backup this process wrote goes (R16)
        static size_t removeOwnedBy(const std::filesystem::path& projectPath, int64_t pid);
    };

}
