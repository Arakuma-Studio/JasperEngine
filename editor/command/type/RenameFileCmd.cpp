// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RenameFileCmd.h"
#include "util/FileUtils.h"

#include "util/Util.h"
#include "asset/AssetOps.h"
#include "Out.h"

using namespace jasperengine;

editor::RenameFileCmd::RenameFileCmd(Project* project, std::string oldName, std::string newName, std::string directory){
    this->project = project;
    this->oldFilename = fs::path(oldName).filename();
    this->newFilename = fs::path(newName).filename();
    this->directory = fs::path(directory);
}

bool editor::RenameFileCmd::renameFile(const fs::path& sourceFs, const fs::path& destFs){
    if (!project) {
        return false;
    }
    const AssetOps::Result result = AssetOps::move(*project, sourceFs, destFs);
    if (!result.ok) {
        Out::error("Couldn't rename \"%s\": %s", sourceFs.filename().string().c_str(), result.error.c_str());
        return false;
    }
    for (const AssetOps::TextReference& broken : result.textReferencesBroken) {
        Out::warning("%s:%u still refers to %s", broken.referrer.c_str(), broken.line, broken.target.c_str());
    }
    return true;
}

bool editor::RenameFileCmd::execute(){
    return renameFile(directory / oldFilename, directory / newFilename);
}

void editor::RenameFileCmd::undo(){
    renameFile(directory / newFilename, directory / oldFilename);
}
