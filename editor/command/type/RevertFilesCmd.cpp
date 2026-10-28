// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RevertFilesCmd.h"

#include "Out.h"
#include "Project.h"
#include "io/AtomicFile.h"
#include "reload/HotReloadService.h"
#include "vcs/VcsService.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    std::atomic<uint64_t> backupCounter{0};

    bool readBytes(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream content;
        content << in.rdbuf();
        out = content.str();
        return static_cast<bool>(in) || in.eof();
    }

}

editor::RevertFilesCmd::RevertFilesCmd(Project* project, fs::path backupDir, std::vector<File> files, size_t reverted)
        : project(project), backupDir(std::move(backupDir)), files(std::move(files)), reverted(reverted) {
}

editor::RevertFilesCmd::~RevertFilesCmd() {
    // the command leaves the history (trimmed, cleared, the project closed): its copies go
    std::error_code ec;
    if (!backupDir.empty()) fs::remove_all(backupDir, ec);
}

fs::path editor::RevertFilesCmd::newBackupDir(const Project& project) {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return project.getVcsBackupDir() /
           (std::to_string(stamp) + "-" + std::to_string(io::currentProcessId()) + "-" + std::to_string(backupCounter++));
}

fs::path editor::RevertFilesCmd::copyPath(const fs::path& backupDir, bool after, size_t index) {
    return backupDir / (after ? "after" : "before") / std::to_string(index);
}

void editor::RevertFilesCmd::reloadChanged(Project* project, const std::vector<fs::path>& paths) {
    if (!project || paths.empty()) return;
    // the assets first (a prefab's instances in the open scenes), quietly: the editor made
    // these changes itself
    HotReloadService& reload = HotReloadService::get();
    if (reload.getProject() == project) {
        ApplyOptions options;
        options.force = true;
        options.quiet = true;
        reload.apply(ChangeSet::fromPaths(paths), options);
    }
    // then the open documents among them read their files again
    std::vector<uint32_t> documents;
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (sceneProject->filepath.empty()) continue;
        fs::path file = sceneProject->filepath;
        if (file.is_relative()) file = project->getProjectPath() / file;
        for (const fs::path& path : paths) {
            if (file.lexically_normal() == path.lexically_normal()) documents.push_back(sceneProject->id);
        }
    }
    for (uint32_t sceneId : documents) {
        std::string error;
        if (!project->reloadSceneFromDisk(sceneId, &error)) Out::error("Revert: %s", error.c_str());
        if (reload.getProject() == project) reload.keepMine(sceneId);
    }
}

bool editor::RevertFilesCmd::restore(bool after) {
    std::vector<fs::path> changed;
    bool ok = true;
    for (size_t i = 0; i < files.size(); i++) {
        const File& file = files[i];
        const bool present = after ? file.after : file.before;
        std::error_code ec;
        if (!present) {
            if (fs::exists(file.path, ec)) {
                fs::remove(file.path, ec);
                if (ec) {
                    Out::error("Couldn't remove \"%s\": %s", file.path.string().c_str(), ec.message().c_str());
                    ok = false;
                    continue;
                }
                changed.push_back(file.path);
            }
            continue;
        }
        std::string bytes;
        if (!readBytes(copyPath(backupDir, after, i), bytes)) {
            Out::error("Couldn't read the revert backup of \"%s\"", file.path.string().c_str());
            ok = false;
            continue;
        }
        // S03: the copy replaces the file atomically (a crash leaves either version)
        const io::WriteResult written = io::writeFileAtomic(file.path, bytes);
        if (!written.ok()) {
            Out::error("Couldn't restore \"%s\": %s", file.path.string().c_str(), io::describeWriteError(written).c_str());
            ok = false;
            continue;
        }
        if (written.written()) changed.push_back(file.path);
    }
    reloadChanged(project, changed);
    if (project) project->getVcs().wake();
    return ok;
}

bool editor::RevertFilesCmd::execute() {
    if (firstRun) {
        // the revert ran already: what it changed reads its files again
        firstRun = false;
        std::vector<fs::path> paths;
        for (const File& file : files) paths.push_back(file.path);
        reloadChanged(project, paths);
        return true;
    }
    return restore(true);
}

void editor::RevertFilesCmd::undo() {
    restore(false);
}

std::string editor::RevertFilesCmd::describe() const {
    return "Revert " + std::to_string(reverted) + (reverted == 1 ? " file" : " files");
}

size_t editor::RevertFilesCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + backupDir.native().size();
    for (const File& file : files) bytes += sizeof(file) + file.path.native().size();
    return bytes;
}
