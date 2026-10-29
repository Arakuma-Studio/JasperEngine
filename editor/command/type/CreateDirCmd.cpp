// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateDirCmd.h"

#include "util/FileUtils.h"
#include "util/SafeWrite.h"

using namespace jasperengine;

editor::CreateDirCmd::CreateDirCmd(std::string dirName, std::string dirPath){
    this->directory = fs::path(dirPath) / fs::path(dirName);
}

std::vector<editor::Command::Touched> editor::CreateDirCmd::touchedPaths(bool) const{
    return {{directory, true}};
}

bool editor::CreateDirCmd::execute(){
    try {
        safewrite::createDirOrThrow(rootFor(directory), directory);
    } catch (const fs::filesystem_error& e) {
        printf("Error: Creating directory %s: %s\n", FileUtils::pathToGenericUtf8(directory).c_str(), e.what());
        return false;
    }

    return true;
}

void editor::CreateDirCmd::undo(){
    try {
        // beneath a root: one removal beneath it, never following a link (a parent swapped for a
        // link since the check: refused, nothing outside removed)
        safewrite::removeTreeOrThrow(rootFor(directory), directory);
    } catch (const fs::filesystem_error& e) {
        printf("Error: Undo creating directory %s: %s\n", FileUtils::pathToGenericUtf8(directory).c_str(), e.what());
        undoProblem = e.what();
    }
}
