// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CopyFileCmd.h"
#include "util/FileUtils.h"
#include "io/AtomicFile.h"

#include "util/Util.h"
#include "asset/AssetOps.h"
#include "Out.h"

using namespace jasperengine;

editor::CopyFileCmd::CopyFileCmd(Project* project, std::vector<std::string> sourceFiles, std::string currentDirectory, std::string targetDirectory, bool copy){
    this->project = project;
    for (const auto& sourceFile : sourceFiles) {
        FileCopyData fdata;
        fdata.filename = fs::path(sourceFile).filename();
        fdata.sourceDirectory = fs::path(currentDirectory);
        fdata.targetDirectory = fs::path(targetDirectory);

        this->files.push_back(fdata);
    }
    this->copy = copy;
}

editor::CopyFileCmd::CopyFileCmd(Project* project, std::vector<std::string> sourcePaths, std::string targetDirectory, bool copy){
    this->project = project;
    for (const auto& sourcePath : sourcePaths) {
        FileCopyData fdata;
        fdata.filename = fs::path(sourcePath).filename();
        fdata.sourceDirectory = fs::path(sourcePath).remove_filename();
        fdata.targetDirectory = fs::path(targetDirectory);

        this->files.push_back(fdata);
    }
    this->copy = copy;
}

namespace {

    // source's bytes replace target's atomically: copied next to it, then published (S03)
    bool replaceFile(const fs::path& source, const fs::path& target, std::string& error) {
        const fs::path temp = io::makeTemporarySiblingPath(target);
        std::error_code ec;
        if (!fs::copy_file(source, temp, ec) || ec) {
            fs::remove(temp, ec);
            error = ec ? ec.message() : "copy failed";
            return false;
        }
        const io::WriteResult published = io::publishFile(temp, target);
        if (!published.ok()) {
            error = io::describeWriteError(published);
            return false;
        }
        return true;
    }

    // Unity's duplicate naming: "hero.png" -> "hero 1.png", "hero 2.png", ...
    fs::path freeCopyName(const fs::path& target) {
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            return target;
        }
        const std::string stem = target.stem().string();
        const std::string extension = target.extension().string();
        for (int counter = 1;; counter++) {
            const fs::path candidate = target.parent_path() / (stem + " " + std::to_string(counter) + extension);
            if (!fs::exists(candidate, ec)) {
                return candidate;
            }
        }
    }

}

// Moves and copies go through AssetOps: files and metas together, GUIDs kept by moves and
// new (the same again on a redo) for copies, references refreshed in memory
bool editor::CopyFileCmd::execute(){
    if (!project) {
        return false;
    }
    for (size_t i = 0; i < files.size(); i++) {
        FileCopyData& fdata = files[i];
        const fs::path source = fdata.sourceDirectory / fdata.filename;
        std::error_code ec;
        if (!fs::exists(source, ec)) {
            continue;
        }
        AssetOps::Result result;
        const fs::path target = fdata.targetDirectory / fdata.filename;
        if (copy && overwrite && fs::is_regular_file(target, ec) && fs::is_regular_file(source, ec)) {
            // replaced in place: the file keeps its meta and GUID, so its references still find it
            fdata.destination = target;
            fdata.backup = AssetOps::uniqueTrashPath(project->getProjectPath() / ".trash", target);
            fs::create_directories(fdata.backup.parent_path(), ec);
            // the backup path is new (uniqueTrashPath): nothing is overwritten
            std::string error;
            if (!fs::copy_file(target, fdata.backup, ec) || ec) {
                error = ec ? ec.message() : "backup failed";
            } else if (!replaceFile(source, target, error)) {
                fs::remove(fdata.backup, ec);
            }
            if (!error.empty()) {
                fdata.backup.clear();
                Out::error("Couldn't replace \"%s\": %s", fdata.filename.string().c_str(), error.c_str());
                return false;
            }
            project->onAssetFilesChanged(project->getAssetDatabase().importAssets(target));
            continue;
        }
        if (copy) {
            if (fdata.destination.empty()) {
                fdata.destination = freeCopyName(fdata.targetDirectory / fdata.filename);
            }
            result = AssetOps::copy(*project, source, fdata.destination, &copyGuids);
        } else {
            fdata.destination = fdata.targetDirectory / fdata.filename;
            result = AssetOps::move(*project, source, fdata.destination);
        }
        if (!result.ok) {
            Out::error("Couldn't %s \"%s\": %s", copy ? "copy" : "move", fdata.filename.string().c_str(), result.error.c_str());
            // what was done goes back, so a failed command leaves nothing half done
            for (size_t back = i; back-- > 0;) {
                const FileCopyData& done = files[back];
                if (copy) {
                    AssetOps::removeCopy(*project, done.destination);
                } else {
                    AssetOps::move(*project, done.destination, done.sourceDirectory / done.filename);
                }
            }
            return false;
        }
    }
    // clips previewing a moved model (or taken from it) name it again, in this step (every file
    // first: a clip moved in the same step is at its final place when its hint is rewritten)
    if (!copy) {
        for (const FileCopyData& fdata : files) {
            if (fdata.destination.empty()) {
                continue;
            }
            std::vector<ClipHintEdit> edits = remapClipHints(project, fdata.sourceDirectory / fdata.filename, fdata.destination);
            clipHintEdits.insert(clipHintEdits.end(), edits.begin(), edits.end());
        }
    }
    return true;
}

void editor::CopyFileCmd::undo(){
    if (!project) {
        return;
    }
    restoreClipHints(project, clipHintEdits);
    clipHintEdits.clear();
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        FileCopyData& fdata = *it;
        std::error_code ec;
        if (fdata.destination.empty() || !fs::exists(fdata.destination, ec)) {
            continue;
        }
        if (!fdata.backup.empty()) {
            // the replaced content comes back, the file kept its identity throughout
            std::string error;
            if (!replaceFile(fdata.backup, fdata.destination, error)) {
                Out::error("Couldn't restore \"%s\": %s", fdata.filename.string().c_str(), error.c_str());
                continue;
            }
            fs::remove(fdata.backup, ec);
            fdata.backup.clear();
            project->onAssetFilesChanged(project->getAssetDatabase().importAssets(fdata.destination));
            continue;
        }
        // instances of a copied prefab made since become Missing Prefabs
        const AssetOps::Result result = copy ? AssetOps::removeCopy(*project, fdata.destination)
                                             : AssetOps::move(*project, fdata.destination, fdata.sourceDirectory / fdata.filename);
        if (!result.ok) {
            Out::error("Couldn't undo the %s of \"%s\": %s", copy ? "copy" : "move", fdata.filename.string().c_str(), result.error.c_str());
        }
    }
}
