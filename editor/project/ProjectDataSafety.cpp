// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project's side of data safety (docs/data-safety.md, docs/specs/S03-data-safety.md):
// the one-editor-per-project lock, the orphan sweep, scene Auto Save and crash recovery.

#include "terrain/TerrainMapPersistence.h"
#include "Project.h"
#include "log/LogFormatCheck.h"

#include "AppSettings.h"
#include "DocumentFile.h"
#include "EditorHost.h"
#include "EditorVersion.h"
#include "Out.h"
#include "Stream.h"
#include "StreamPreserved.h"
#include "Workspace.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "util/FileUtils.h"
#include "voxel/VoxelAssetFile.h"

#include "json.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string hostName() {
#if defined(_WIN32)
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(name, &size)) return std::string();
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
        if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), utf8.data(), bytes, nullptr, nullptr);
        return utf8;
#else
        char name[256] = {};
        return gethostname(name, sizeof(name) - 1) == 0 ? std::string(name) : std::string();
#endif
    }

    JE_STRFTIME_FORMAT(2) std::string localClock(std::time_t time, const char* format) {
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char buffer[40];
        std::strftime(buffer, sizeof(buffer), format, &local);
        return buffer;
    }

    // JE_AUTOSAVE_INTERVAL_TEST=1 (tests): backups every 2 s instead of the setting's interval
    float configuredAutosaveInterval() {
        if (const char* test = std::getenv("JE_AUTOSAVE_INTERVAL_TEST"); test && std::string(test) == "1") {
            return 2.0f;
        }
        return (float)AppSettings::getAutosaveIntervalSeconds();
    }

    constexpr float kSlowEncodeSeconds = 0.05f;

}

// ---- the project lock (R10) ----

void editor::Project::setIgnoreProjectLock(bool ignore) {
    ignoreProjectLock = ignore;
}

void editor::Project::setSkipProjectLock(bool skip) {
    skipProjectLock = skip;
}

bool editor::Project::wasProjectLocked() const {
    return projectLockedFailure;
}

const std::string& editor::Project::getProjectLockMessage() const {
    return projectLockMessage;
}

fs::path editor::Project::getProjectLockPath() const {
    return Workspace::getUserDirectory(projectPath) / "editor.lock";
}

bool editor::Project::holdsProjectLock() const {
    return projectLock && projectLock->isLocked();
}

bool editor::Project::takeProjectLock(const fs::path& path) {
    projectLockedFailure = false;
    projectLockMessage.clear();
    if (skipProjectLock) {
        projectLock.reset();
        return true;
    }
    const fs::path lockFile = Workspace::getUserDirectory(path) / "editor.lock";
    // the project this editor had open keeps its lock until the new one is taken; reopening
    // the same project keeps the lock it holds
    if (projectLock && projectLock->isLocked() && projectLock->getPath() == lockFile) {
        return true;
    }

    auto lock = std::make_unique<io::ScopedFileLock>(lockFile, io::ScopedFileLock::Mode::Exclusive, false);
    if (lock->isLocked()) {
        nlohmann::ordered_json owner;
        owner["pid"] = io::currentProcessId();
        owner["host"] = hostName();
        owner["started"] = localClock(std::time(nullptr), "%Y-%m-%dT%H:%M:%S");
        owner["editor_version"] = JASPERENGINE_EDITOR_VERSION;
        lock->writeOwnerInfo(owner.dump(2) + "\n");
        projectLock = std::move(lock);
        return true;
    }

    if (lock->getStatus() != io::ScopedFileLock::Status::Busy) {
        // a share without locks, a read-only volume: one warning, then on without the lock
        Out::warning("Project lock unavailable (%s): two editors opening this project will not be stopped.",
                     lock->failureReason().c_str());
        projectLock.reset();
        return true;
    }

    ProjectLockInfo info;
    info.projectPath = path;
    info.projectName = path.filename().string();
    try {
        const nlohmann::json owner = nlohmann::json::parse(io::ScopedFileLock::readOwnerInfo(lockFile));
        info.pid = owner.value("pid", (int64_t)0);
        info.host = owner.value("host", std::string());
        info.editorVersion = owner.value("editor_version", std::string());
        const std::string started = owner.value("started", std::string());
        // "2026-10-23T09:12:00" shows as 09:12
        info.since = started.size() >= 16 ? started.substr(11, 5) : started;
    } catch (const std::exception&) {
    }
    projectLockMessage = "\"" + info.projectName + "\" is already open in another JasperEngine editor (PID " +
            std::to_string(info.pid) + " on host \"" + info.host + "\"" + (info.since.empty() ? std::string() : ", since " + info.since) +
            "). Two editors on one project overwrite each other's changes.";

    if (ignoreProjectLock) {
        Out::warning("%s Opened anyway (--ignore-lock).", projectLockMessage.c_str());
        projectLock.reset();
        return true;
    }
    info.openAnyway = [this, path]() {
        editor::getEditorHost().requestProjectChange([this, path]() {
            const bool previous = ignoreProjectLock;
            ignoreProjectLock = true;
            loadProject(path);
            ignoreProjectLock = previous;
        });
    };
    if (editor::getEditorHost().confirmOpenLockedProject(info)) {
        projectLock.reset();
        return true;
    }
    projectLockedFailure = true;
    Out::error("project_locked: %s", projectLockMessage.c_str());
    return false;
}

// ---- the orphan sweep (R12) ----

void editor::Project::sweepOrphanedTemporaryFiles() {
    // the asset database walked the project already; .jasperengine is skipped by it
    std::vector<fs::path> candidates = assetDatabase.takeTemporaryFiles();
    const fs::path internal = projectPath / ".jasperengine";
    const fs::path quarantine = SceneBackups::root(projectPath) / "orphans";
    std::error_code ec;
    if (fs::is_directory(internal, ec)) {
        for (fs::recursive_directory_iterator it(internal, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                // the generated build trees hold no documents
                const std::string name = it->path().filename().string();
                if (name == "build" || name == "externalbuild" || it->path() == quarantine)
                    it.disable_recursion_pending();
                continue;
            }
            if (io::isTemporarySiblingName(it->path().filename().string()))
                candidates.push_back(it->path());
        }
    }
    io::pruneQuarantine(quarantine);
    if (candidates.empty()) {
        return;
    }
    const io::OrphanSweep sweep = io::quarantineOrphanedTemporaryFiles(candidates, quarantine, projectPath);
    if (!sweep.quarantined.empty()) {
        std::string list;
        for (const fs::path& moved : sweep.quarantined) {
            list += "\n  " + FileUtils::pathToUtf8(moved.lexically_relative(projectPath));
        }
        Out::warning("A previous session ended in the middle of %zu save(s); what it was writing is kept in %s (deleted after 7 days):%s",
                     sweep.quarantined.size(), FileUtils::pathToUtf8(quarantine.lexically_relative(projectPath)).c_str(), list.c_str());
    }
}

// ---- Auto Save (R13-R15) ----

float editor::Project::getAutosaveInterval(uint32_t sceneId) const {
    auto it = autosaveScenes.find(sceneId);
    return it != autosaveScenes.end() && it->second.interval > 0.0f ? it->second.interval : configuredAutosaveInterval();
}

// the temporary project the editor makes for itself (<temp>/<name>, deleted when the next one is
// made): backups there would not survive the restart they are for. Any other project below the
// temp directory (a test's scratch project, a checkout in /tmp) is a project like any other.
bool editor::Project::isEditorTempProject() const {
    if (!isTempProject()) {
        return false;
    }
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec);
    if (ec) {
        return false;
    }
    const bool same = fs::equivalent(projectPath.parent_path(), temp, ec);
    return same && !ec;
}

void editor::Project::tickAutosave(float deltaSeconds, bool userInteracting) {
    // with nothing to do this is the whole cost of a frame
    autosaveClock += deltaSeconds;
    autosaveLastTick = std::chrono::steady_clock::now();
    if (autosaveClock < autosaveNextCheck) {
        return;
    }
    if (userInteracting) {
        autosaveNextCheck = autosaveClock;  // the next frame
        return;
    }
    autosaveNextCheck = autosaveClock + 1.0f;
    if (!AppSettings::isAutosaveEnabled() || !modelLoaded || projectPath.empty() || isEditorTempProject() || isAnyScenePlaying()) {
        return;
    }

    const float interval = configuredAutosaveInterval();
    // T1 R30: what a backup refers to should be on disk by the time a crash needs it
    TerrainMapPersistence::get().handOverAll();
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.opened && sceneProject.scene) {
            autosaveDocument(sceneProject, sceneProject.filepath.empty() ? BackupKind::Untitled : BackupKind::Scene,
                             sceneProject.filepath, interval);
        }
    }
    // Prefab Mode writes the asset itself while Auto Save is on
    if (!prefabAutoSave) {
        for (const auto& stage : prefabStages) {
            if (hasUnsavedPrefabStage(stage->sceneProject.id) && !isPrefabStageDegraded(stage->sceneProject.id)) {
                autosaveDocument(stage->sceneProject, BackupKind::Prefab, stage->assetPath, interval);
            }
        }
    }
}

bool editor::Project::isAutosaveDue() const {
    if (!AppSettings::isAutosaveEnabled() || !modelLoaded || projectPath.empty() || isEditorTempProject() || isAnyScenePlaying()) {
        return false;
    }
    // the clock as the next frame's tick will see it
    const float now = autosaveClock + std::chrono::duration<float>(std::chrono::steady_clock::now() - autosaveLastTick).count();
    if (now < autosaveNextCheck) {
        return false;
    }
    auto due = [&](uint32_t sceneId) {
        auto it = autosaveScenes.find(sceneId);
        // a change not seen yet starts its interval
        if (it == autosaveScenes.end() || it->second.interval <= 0.0f) return true;
        if (now - it->second.lastWrite < it->second.interval) return false;
        // (a backup of it still being written is the next tick's)
        std::lock_guard<std::mutex> lock(it->second.written->mutex);
        return !it->second.written->writing;
    };
    // the same scenes autosaveDocument backs up
    for (const SceneProject& sceneProject : scenes) {
        if (sceneProject.opened && sceneProject.scene && sceneProject.isModified &&
                sceneProject.playState == ScenePlayState::STOPPED && sceneProject.loadReport.dropped() == 0 && due(sceneProject.id)) {
            return true;
        }
    }
    if (!prefabAutoSave) {
        for (const auto& stage : prefabStages) {
            if (hasUnsavedPrefabStage(stage->sceneProject.id) && !isPrefabStageDegraded(stage->sceneProject.id) && due(stage->sceneProject.id)) {
                return true;
            }
        }
    }
    return false;
}

void editor::Project::autosaveDocument(SceneProject& sceneProject, BackupKind kind, const fs::path& source, float interval) {
    const bool prefab = kind == BackupKind::Prefab;
    if (!prefab && (!sceneProject.isModified || sceneProject.playState != ScenePlayState::STOPPED)) {
        return;
    }
    // a scene that did not load completely gets no backup: restoring it would write a file
    // that silently lacks what was dropped
    if (sceneProject.loadReport.dropped() > 0) {
        return;
    }
    AutosaveScene& state = autosaveScenes[sceneProject.id];
    if (state.interval <= 0.0f) {
        state.interval = interval;
        state.lastWrite = autosaveClock;
        return;  // the first backup an interval after the first change seen
    }
    if (autosaveClock - state.lastWrite < state.interval) {
        return;
    }
    state.lastWrite = autosaveClock;

    // a backup of this scene still being written: the next tick
    {
        std::lock_guard<std::mutex> lock(state.written->mutex);
        if (state.written->writing) {
            return;
        }
    }

    // encoded here, on the main thread, without the pre-save steps of a save (nothing of the
    // scene changes); turned into text, compared with the last backup and written on a worker
    const auto start = std::chrono::steady_clock::now();
    YAML::Node root;
    std::string baseSha1;
    try {
        if (prefab) {
            PrefabStage* stage = getPrefabStage(sceneProject.id);
            if (!stage) return;
            root = encodePrefabStage(*stage);
            if (!root || !root.IsMap()) return;
            auto asset = prefabAssets.find(stage->assetPath);
            baseSha1 = asset != prefabAssets.end() ? asset->second.diskSha1 : std::string();
        } else {
            root = Stream::encodeSceneProject(this, &sceneProject);
            baseSha1 = sceneProject.diskSha1;
        }
    } catch (const std::exception& e) {
        Out::warning("Auto Save skipped \"%s\": %s", sceneProject.name.c_str(), e.what());
        return;
    }
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    if (seconds > kSlowEncodeSeconds && state.interval < (float)AppSettings::maxAutosaveInterval) {
        state.interval = std::min(state.interval * 2.0f, (float)AppSettings::maxAutosaveInterval);
        Out::info("Auto Save of \"%s\" took %.0f ms to encode: its interval is now %.0f s.",
                  sceneProject.name.c_str(), seconds * 1000.0f, state.interval);
    }

    // T6 R77: its dirty Voxel Volumes go beside it; encoded here, written with it
    std::vector<VoxelBackup> voxelBackups;
    if (!prefab) {
        voxelBackups = collectVoxelBackups(*this, sceneProject, state.voxelVersions);
    }

    const fs::path project = projectPath;
    const uint32_t sceneId = sceneProject.id;
    const int version = StreamMigrations::currentVersion(prefab ? DocumentKind::Prefab : DocumentKind::Scene);
    std::shared_ptr<AutosaveScene::Written> written = state.written;
    {
        std::lock_guard<std::mutex> lock(written->mutex);
        written->writing = true;
    }
    std::shared_ptr<std::atomic<int>> inFlight = autosaveWritesInFlight;
    inFlight->fetch_add(1);
    // the flag and the count drop when the write ends, or when the job is dropped unrun (the
    // job system cancels jobs not started at shutdown)
    std::shared_ptr<void> finished(nullptr, [written, inFlight](void*) {
        {
            std::lock_guard<std::mutex> lock(written->mutex);
            written->writing = false;
        }
        inFlight->fetch_sub(1);
    });
    // a disk write: the io-write channel (one at a time, in order with the scene saves)
    jobs::SubmitOptions options;
    options.name = "autosave-write";
    jobs::system().ioWriteChannel().submit([project, kind, source, sceneId, root, baseSha1, version, prefab, written, finished, voxelBackups]() {
        // the volumes first: a scene backup always names complete copies
        writeVoxelBackups(project, voxelBackups);
        // the tree belongs to this thread now: nothing else holds its nodes
        const std::string text = (prefab ? YAML::Dump(root) : DocumentFiles::text(root)) + voxelBackupMarkers(voxelBackups);
        const std::string sha1 = DocumentFiles::sha1(text);
        bool unchanged;
        {
            std::lock_guard<std::mutex> lock(written->mutex);
            unchanged = sha1 == written->sha1;
        }
        // only when the bytes differ from the last backup
        if (!unchanged && SceneBackups::write(project, kind, source, sceneId, text, baseSha1, version)) {
            std::lock_guard<std::mutex> lock(written->mutex);
            written->sha1 = sha1;
        }
    }, options);
}

void editor::Project::deleteSceneBackup(const SceneProject& sceneProject, const fs::path& previousPath) {
    if (projectPath.empty()) {
        return;
    }
    if (previousPath.empty()) {
        SceneBackups::discard(projectPath, BackupKind::Untitled, {}, sceneProject.id);
    } else {
        SceneBackups::discard(projectPath, BackupKind::Scene, previousPath);
    }
    if (!sceneProject.filepath.empty() && sceneProject.filepath != previousPath) {
        SceneBackups::discard(projectPath, BackupKind::Scene, sceneProject.filepath);
    }
    // the autosave state belongs to the main thread (a save may run on a worker)
    const uint32_t sceneId = sceneProject.id;
    if (editor::getEditorHost().isMainThread()) {
        autosaveScenes.erase(sceneId);
    } else {
        editor::getEditorHost().enqueueMainThreadTask([this, sceneId]() {
            autosaveScenes.erase(sceneId);
        });
    }
}

bool editor::Project::warnPreservedNotInBuild(const SceneProject& sceneProject) const {
    const PreservedStore* store = findPreservedStore(sceneProject.scene);
    const size_t count = store ? store->preservedComponentCount() : 0;
    if (count == 0) {
        return false;
    }
    const std::string name = sceneProject.filepath.empty() ? sceneProject.name : sceneProject.filepath.filename().string();
    Out::warning("%s: %zu component%s this editor does not know %s kept in the file but not in the build.",
                 name.c_str(), count, count == 1 ? "" : "s", count == 1 ? "is" : "are");
    return true;
}

// ---- crash recovery (R16-R19) ----

void editor::Project::setNextRecoveryChoice(RecoveryChoice choice) {
    nextRecoveryChoice = choice;
}

const editor::RecoveryOutcome& editor::Project::getLastRecovery() const {
    return lastRecovery;
}

bool editor::Project::resolveSceneRecovery(const fs::path& relativePath, std::string& restoredBytes) {
    lastRecovery = RecoveryOutcome();
    lastRecovery.checked = true;
    const std::optional<RecoveryChoice> forced = nextRecoveryChoice;
    nextRecoveryChoice.reset();

    lastRecovery.backup = SceneBackups::scan(projectPath, relativePath);
    if (lastRecovery.backup.status != BackupStatus::Usable) {
        return false;
    }
    // never through the alert defaults: the agent's argument, else the host (Keep headless)
    const RecoveryChoice choice = forced ? *forced : editor::getEditorHost().resolveSceneRecovery(lastRecovery.backup);
    lastRecovery.applied = choice;
    if (choice == RecoveryChoice::Discard) {
        std::string backupText;
        if (SceneBackups::readBackup(lastRecovery.backup, backupText)) discardVoxelBackups(projectPath, backupText);
        SceneBackups::discard(lastRecovery.backup.backup);
        Out::info("Discarded the auto-saved changes of \"%s\".", relativePath.generic_string().c_str());
        return false;
    }
    if (choice == RecoveryChoice::Restore && SceneBackups::readBackup(lastRecovery.backup, restoredBytes)) {
        // before the scene loads: its VoxelSystem finds these instead of reading the files
        restoredVoxelAssets = restoreVoxelBackups(*this, restoredBytes);
        Out::info("Restored \"%s\" from its auto-saved copy (%s); save to keep it.",
                  relativePath.generic_string().c_str(), lastRecovery.backup.savedAt.c_str());
        return true;
    }
    return false;
}

std::vector<SceneBackupInfo> editor::Project::listSceneBackups() const {
    return projectPath.empty() ? std::vector<SceneBackupInfo>() : SceneBackups::list(projectPath);
}

bool editor::Project::discardSceneBackup(uint32_t sceneId) {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    std::string backupText;
    if (DocumentFiles::readBytes(SceneBackups::backupPath(projectPath, sceneProject->filepath.empty() ? BackupKind::Untitled : BackupKind::Scene,
                                                          sceneProject->filepath, sceneId), backupText)) {
        discardVoxelBackups(projectPath, backupText);
    }
    const bool removed = sceneProject->filepath.empty() ?
            SceneBackups::discard(projectPath, BackupKind::Untitled, {}, sceneId) :
            SceneBackups::discard(projectPath, BackupKind::Scene, sceneProject->filepath);
    autosaveScenes.erase(sceneId);
    return removed;
}

bool editor::Project::discardSceneBackup(const fs::path& scenePath) {
    return SceneBackups::discard(projectPath, BackupKind::Scene, normalizeToProjectRelative(scenePath));
}

bool editor::Project::restoreSceneFromBackup(uint32_t sceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || sceneProject->filepath.empty() || isAnyScenePlaying()) {
        return false;
    }
    const fs::path path = sceneProject->filepath;
    const bool selected = getSelectedSceneId() == sceneId;
    // reopened with the backup's content: a fresh scene, its undo history empty
    closeScene(sceneId, true);
    setNextRecoveryChoice(RecoveryChoice::Restore);
    const bool loaded = loadScene(getProjectPath() / path, true, false, true);
    nextRecoveryChoice.reset();
    if (loaded && selected) {
        const uint32_t reopened = findSceneByPath(getProjectPath() / path);
        if (reopened != NULL_PROJECT_SCENE) setSelectedSceneId(reopened);
    }
    saveWorkspaceFile();
    return loaded && lastRecovery.applied == RecoveryChoice::Restore;
}

void editor::Project::offerUntitledBackups() {
    for (const SceneBackupInfo& backup : SceneBackups::orphanedUntitled(projectPath)) {
        const RecoveryChoice choice = editor::getEditorHost().resolveSceneRecovery(backup);
        if (choice == RecoveryChoice::Discard) {
            SceneBackups::discard(backup.backup);
        } else if (choice == RecoveryChoice::Restore) {
            restoreUntitledBackup(backup);
        }
    }
}

bool editor::Project::restoreUntitledBackup(const SceneBackupInfo& backup) {
    std::string bytes;
    if (!SceneBackups::readBackup(backup, bytes)) {
        return false;
    }
    const uint32_t id = loadUntitledScene(bytes, backup.backup.filename().string());
    if (id == NULL_PROJECT_SCENE) {
        return false;
    }
    // the restored scene backs up under its own id from now on
    if (id != backup.sceneId) {
        SceneBackups::discard(backup.backup);
    }
    return true;
}

void editor::Project::endProjectSession() {
    if (!projectPath.empty()) {
        // a backup still being written would come back after the cleanup
        for (int i = 0; i < 200 && autosaveWritesInFlight->load() > 0; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        SceneBackups::removeOwnedBy(projectPath, io::currentProcessId());
    }
    autosaveScenes.clear();
    projectLock.reset();
}
