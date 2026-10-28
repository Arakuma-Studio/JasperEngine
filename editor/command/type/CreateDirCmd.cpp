// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateDirCmd.h"

#include "util/SafeWrite.h"

using namespace jasperengine;

editor::CreateDirCmd::CreateDirCmd(std::string dirName, std::string dirPath){
    this->directory = fs::path(dirPath) / fs::path(dirName);
}

bool editor::CreateDirCmd::execute(){
    try {
        safewrite::createDirOrThrow(beneathRoot, directory);
    } catch (const fs::filesystem_error& e) {
        printf("Error: Creating directory %s: %s\n", directory.string().c_str(), e.what());
        return false;
    }

    return true;
}

void editor::CreateDirCmd::undo(){
    try {
        // beneath a root: never through a link made since (the folder itself a link: left alone)
        std::error_code ec;
        if (!beneathRoot.empty() && (fs::is_symlink(directory, ec) || !safewrite::escapeOf(beneathRoot, directory, true).empty())) return;
        fs::remove_all(directory);
    } catch (const fs::filesystem_error& e) {
        printf("Error: Undo creating directory %s: %s\n", directory.string().c_str(), e.what());
    }
}