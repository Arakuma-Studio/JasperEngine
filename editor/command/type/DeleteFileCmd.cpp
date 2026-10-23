// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeleteFileCmd.h"
#include "util/FileUtils.h"

#include "util/Util.h"
#include "asset/AssetOps.h"
#include "Out.h"

#include <filesystem>
#include <string>

using namespace jasperengine;

editor::DeleteFileCmd::DeleteFileCmd(Project* project, std::vector<fs::path> filePaths, fs::path rootPath){
    this->project = project;
    for (fs::path& file : filePaths){
        this->files.push_back({file, fs::path()});
    }
    this->trash = rootPath / ".trash";
}

// Deleting moves the files (metas included) to the project trash through AssetOps: the
// references to them stay and show as Missing, and an undo resolves them again (S04 R15)
bool editor::DeleteFileCmd::execute(){
    if (!project) {
        return false;
    }
    for (DeleteFilesData& file : files){
        std::error_code ec;
        if (!fs::exists(file.originalFile, ec)) {
            continue;
        }
        // what is already in the trash is deleted for good
        const fs::path relToTrash = file.originalFile.lexically_normal().lexically_relative(trash.lexically_normal());
        if (file.originalFile == trash || (!relToTrash.empty() && *relToTrash.begin() != "..")) {
            fs::remove_all(file.originalFile, ec);
            file.trashFile.clear();
            continue;
        }
        const AssetOps::Result result = AssetOps::remove(*project, file.originalFile, file.trashFile);
        if (!result.ok) {
            Out::error("Couldn't delete \"%s\": %s", file.originalFile.filename().string().c_str(), result.error.c_str());
            // the ones deleted before come back: a failed command leaves nothing half done
            for (DeleteFilesData& done : files) {
                if (&done == &file) break;
                if (!done.trashFile.empty()) AssetOps::restore(*project, done.trashFile, done.originalFile);
            }
            return false;
        }
        for (const AssetOps::TextReference& broken : result.textReferencesBroken) {
            Out::warning("%s:%u refers to the deleted %s", broken.referrer.c_str(), broken.line, broken.target.c_str());
        }
    }
    return true;
}

void editor::DeleteFileCmd::undo(){
    if (!project) {
        return;
    }
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        DeleteFilesData& file = *it;
        std::error_code ec;
        // only what went to the trash comes back (not what was deleted for good)
        if (file.trashFile.empty() || !fs::exists(file.trashFile, ec)) {
            continue;
        }
        const AssetOps::Result result = AssetOps::restore(*project, file.trashFile, file.originalFile);
        if (!result.ok) {
            Out::error("Couldn't restore \"%s\": %s", file.originalFile.filename().string().c_str(), result.error.c_str());
        }
    }
}
