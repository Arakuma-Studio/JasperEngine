// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneBackup.h"
#include "log/LogFormatCheck.h"

#include "DocumentFile.h"
#include "EditorVersion.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <system_error>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // "scenes/Level1.scene" on every platform
    std::string genericUtf8(const fs::path& path) {
        std::string text = FileUtils::pathToUtf8(path);
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    }

    const char* kindDirectory(BackupKind kind) {
        switch (kind) {
            case BackupKind::Scene: return "scenes";
            case BackupKind::Untitled: return "untitled";
            case BackupKind::Prefab: return "prefabs";
            case BackupKind::Voxel: return "voxels";
        }
        return "scenes";
    }

    std::tm localTime(std::time_t time) {
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        return local;
    }

    JE_STRFTIME_FORMAT(2) std::string formatTime(std::time_t time, const char* format) {
        const std::tm local = localTime(time);
        char buffer[40];
        std::strftime(buffer, sizeof(buffer), format, &local);
        return buffer;
    }

    // backup/stale/<relative path>.<timestamp>, free of collisions
    fs::path stalePath(const fs::path& projectPath, const fs::path& source) {
        const fs::path base = SceneBackups::root(projectPath) / "stale" / source;
        const std::string stamp = formatTime(std::time(nullptr), "%Y%m%d-%H%M%S");
        fs::path candidate = base;
        candidate += "." + stamp;
        std::error_code ec;
        for (int n = 1; fs::exists(candidate, ec) && n < 1000; n++) {
            candidate = base;
            candidate += "." + stamp + "-" + std::to_string(n);
        }
        return candidate;
    }

    bool moveFile(const fs::path& from, const fs::path& to) {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        return io::renameWithRetry(from, to, ec);
    }

    BackupKind kindOfDirectory(const std::string& name) {
        if (name == "untitled") return BackupKind::Untitled;
        if (name == "prefabs") return BackupKind::Prefab;
        return BackupKind::Scene;
    }

}

std::string SceneBackupInfo::savedAtClock() const {
    return savedAtEpoch > 0 ? formatTime((std::time_t)savedAtEpoch, "%H:%M:%S") : std::string();
}

fs::path SceneBackups::root(const fs::path& projectPath) {
    return projectPath / ".jasperengine" / "backup";
}

fs::path SceneBackups::backupPath(const fs::path& projectPath, BackupKind kind, const fs::path& source, uint32_t sceneId) {
    if (kind == BackupKind::Untitled)
        return root(projectPath) / kindDirectory(kind) / (std::to_string(sceneId) + ".scene");
    return root(projectPath) / kindDirectory(kind) / source.lexically_normal();
}

fs::path SceneBackups::sidecarPath(const fs::path& backup) {
    fs::path sidecar = backup;
    sidecar += ".json";
    return sidecar;
}

bool SceneBackups::write(const fs::path& projectPath, BackupKind kind, const fs::path& source, uint32_t sceneId,
        const std::string& bytes, const std::string& baseSha1, int formatVersion) {
    const fs::path backup = backupPath(projectPath, kind, source, sceneId);
    const std::time_t now = std::time(nullptr);
    nlohmann::ordered_json sidecar;
    sidecar["source"] = genericUtf8(source);
    sidecar["base_sha1"] = baseSha1;
    sidecar["saved_at"] = formatTime(now, "%Y-%m-%dT%H:%M:%S");
    sidecar["saved_at_epoch"] = (int64_t)now;
    sidecar["format_version"] = formatVersion;
    sidecar["editor_version"] = JASPERENGINE_EDITOR_VERSION;
    sidecar["pid"] = io::currentProcessId();
    if (kind == BackupKind::Untitled)
        sidecar["scene_id"] = sceneId;

    // the backup first: a sidecar always describes a complete backup
    io::WriteResult written = DocumentFiles::write(backup, bytes);
    if (written.ok())
        written = DocumentFiles::write(sidecarPath(backup), sidecar.dump(2) + "\n");
    if (!written.ok()) {
        Out::warning("Auto Save could not write \"%s\": %s", FileUtils::pathToUtf8(backup).c_str(), io::describeWriteError(written).c_str());
        return false;
    }
    return true;
}

bool SceneBackups::readSidecar(const fs::path& backup, SceneBackupInfo& info) {
    std::string text;
    if (!DocumentFiles::readBytes(sidecarPath(backup), text))
        return false;
    try {
        const nlohmann::json sidecar = nlohmann::json::parse(text);
        info.backup = backup;
        info.source = FileUtils::pathFromUtf8(sidecar.value("source", std::string()));
        info.baseSha1 = sidecar.value("base_sha1", std::string());
        info.savedAt = sidecar.value("saved_at", std::string());
        info.savedAtEpoch = sidecar.value("saved_at_epoch", (int64_t)0);
        info.formatVersion = sidecar.value("format_version", 0);
        info.editorVersion = sidecar.value("editor_version", std::string());
        info.pid = sidecar.value("pid", (int64_t)0);
        info.sceneId = sidecar.value("scene_id", 0u);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool SceneBackups::readBackup(const SceneBackupInfo& info, std::string& bytes) {
    return DocumentFiles::readBytes(info.backup, bytes);
}

bool SceneBackups::discard(const fs::path& backup) {
    std::error_code ec;
    const bool removed = fs::remove(backup, ec);
    fs::remove(sidecarPath(backup), ec);
    return removed;
}

bool SceneBackups::discard(const fs::path& projectPath, BackupKind kind, const fs::path& source, uint32_t sceneId) {
    return discard(backupPath(projectPath, kind, source, sceneId));
}

SceneBackupInfo SceneBackups::scan(const fs::path& projectPath, const fs::path& source, BackupKind kind) {
    SceneBackupInfo info;
    info.kind = kind;
    info.source = source;
    const fs::path backup = backupPath(projectPath, kind, source);
    std::error_code ec;
    if (!fs::is_regular_file(backup, ec) || !readSidecar(backup, info)) {
        return info;
    }
    info.source = source;
    // a running editor (another instance, or this one) is still working with it
    if (info.pid == io::currentProcessId() || io::isProcessRunning(info.pid)) {
        info.status = BackupStatus::None;
        return info;
    }

    std::string fileBytes;
    const bool fileExists = DocumentFiles::readBytes(projectPath / source, fileBytes);
    const std::string fileSha1 = fileExists ? DocumentFiles::sha1(fileBytes) : std::string();
    if (!fileExists || fileSha1 != info.baseSha1) {
        // the file changed since the backup was made (a pull, another editor): set aside
        const fs::path target = stalePath(projectPath, source);
        if (moveFile(backup, target)) {
            moveFile(sidecarPath(backup), sidecarPath(target));
            info.backup = target;
            Out::warning("The auto-saved copy of \"%s\" is older than the file and was set aside: %s",
                         genericUtf8(source).c_str(), FileUtils::pathToUtf8(target).c_str());
        }
        info.status = BackupStatus::Stale;
        return info;
    }

    std::string backupBytes;
    if (!DocumentFiles::readBytes(backup, backupBytes) || backupBytes == fileBytes) {
        // nothing in it the file does not have
        discard(backup);
        info.status = BackupStatus::None;
        return info;
    }
    info.status = BackupStatus::Usable;
    return info;
}

std::vector<SceneBackupInfo> SceneBackups::list(const fs::path& projectPath) {
    std::vector<SceneBackupInfo> backups;
    for (const char* directory : {"scenes", "untitled", "prefabs"}) {
        const fs::path base = root(projectPath) / directory;
        std::error_code ec;
        if (!fs::is_directory(base, ec))
            continue;
        for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (!it->is_regular_file(entryEc) || it->path().extension() == ".json" || io::isTemporarySiblingName(it->path().filename().string()))
                continue;
            SceneBackupInfo info;
            if (!readSidecar(it->path(), info))
                continue;
            info.kind = kindOfDirectory(directory);
            if (info.kind == BackupKind::Untitled) {
                info.status = io::isProcessRunning(info.pid) && info.pid != io::currentProcessId() ? BackupStatus::None : BackupStatus::Usable;
            } else if (info.pid != io::currentProcessId() && io::isProcessRunning(info.pid)) {
                info.status = BackupStatus::None;
            } else {
                const std::string fileSha1 = DocumentFiles::fileSha1(projectPath / info.source);
                info.status = fileSha1 == info.baseSha1 ? BackupStatus::Usable : BackupStatus::Stale;
            }
            backups.push_back(std::move(info));
        }
    }
    return backups;
}

std::vector<SceneBackupInfo> SceneBackups::orphanedUntitled(const fs::path& projectPath) {
    std::vector<SceneBackupInfo> orphans;
    for (SceneBackupInfo& info : list(projectPath)) {
        if (info.kind == BackupKind::Untitled && info.pid != io::currentProcessId() && !io::isProcessRunning(info.pid))
            orphans.push_back(std::move(info));
    }
    return orphans;
}

size_t SceneBackups::removeOwnedBy(const fs::path& projectPath, int64_t pid) {
    size_t removed = 0;
    for (const SceneBackupInfo& info : list(projectPath)) {
        if (info.pid == pid && discard(info.backup))
            removed++;
    }
    // a volume's copies are its scene's, and no listing shows them
    const fs::path voxels = root(projectPath) / kindDirectory(BackupKind::Voxel);
    std::error_code ec;
    std::vector<fs::path> copies;
    if (fs::is_directory(voxels, ec)) {
        for (fs::recursive_directory_iterator it(voxels, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_regular_file(entryEc) && it->path().extension() != ".json" && !io::isTemporarySiblingName(it->path().filename().string()))
                copies.push_back(it->path());
        }
    }
    for (const fs::path& copy : copies) {
        SceneBackupInfo info;
        if (readSidecar(copy, info) && info.pid == pid && discard(copy))
            removed++;
    }
    return removed;
}
