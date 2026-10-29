// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeleteFileCmd.h"

#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "util/Util.h"
#include "vat/VatAssetFiles.h"

#include <filesystem>
#include <string>

using namespace jasperengine;

editor::DeleteFileCmd::DeleteFileCmd(Project* project, std::vector<fs::path> filePaths, fs::path rootPath){
    this->project = project;
    // a Vertex Animation goes as a pair: the .vat with its generated .vat.glb (spec 2.1)
    for (const fs::path& file : vat::withPairs(filePaths)){
        this->files.push_back({file, fs::path()});
    }
    this->trash = rootPath / ".trash";
}

fs::path editor::DeleteFileCmd::generateUniqueTrashPath(const fs::path& trashDir, const fs::path& originalFile) {
    fs::path basePath = trashDir / originalFile.filename();
    fs::path extension = originalFile.extension();
    fs::path stemPath = basePath.stem();
    
    if (!fs::exists(basePath)) {
        return basePath;
    }

    int counter = 1;
    fs::path newPath;
    do {
        newPath = trashDir / (stemPath.string() + " (" + std::to_string(counter) + ")" + extension.string());
        counter++;
    } while (fs::exists(newPath));

    return newPath;
}

std::vector<editor::Command::Touched> editor::DeleteFileCmd::touchedPaths(bool undo) const{
    std::vector<Touched> out;
    for (const DeleteFilesData& file : files){
        if (undo){
            if (file.trashFile.empty()) continue;
            out.push_back({file.trashFile, true});
        }
        out.push_back({file.originalFile, true});
    }
    if (!undo) out.push_back({trash, true});
    return out;
}

bool editor::DeleteFileCmd::execute(){
    // a closed scene whose file goes leaves the project (Project::cleanupSceneFilePath)
    refusal.clear();
    if (project) {
        std::vector<fs::path> paths;
        for (const DeleteFilesData& file : files) {
            paths.push_back(file.originalFile);
        }
        if (project->refuseDeletionWhilePlaying(paths, &refusal, alertOnRefusal)) {
            return false;
        }
    }

    try {
        if (!fs::exists(trash)) {
            // the editor's own .trash (a hidden folder an MCP call's deny-by-default guard refuses):
            // named so its own creation goes through (util/SafeWrite.h, EditorInternal)
            safewrite::EditorInternal trashDir(trash);
            safewrite::createDirOrThrow(rootFor(trash), trash);
        }
    } catch (const fs::filesystem_error& e) {
        printf("Error: Creating %s: %s\n", FileUtils::pathToGenericUtf8(trash).c_str(), e.what());
        return false;
    }

    std::vector<fs::path> deletedPaths;
    bool deletedShaderSource = false;

    for (DeleteFilesData& file : files){
        try {
            if (fs::exists(file.originalFile)) {
                const bool isDirectory = fs::is_directory(file.originalFile);
                deletedShaderSource |= isDirectory || Util::isShaderFile(file.originalFile.string());
                // Check if file is inside .trash directory or is .trash directory
                const fs::path relToTrash = fs::relative(file.originalFile, trash);
                if (file.originalFile == trash ||
                    (!relToTrash.empty() && *relToTrash.begin() != "..")) {
                    // a permanent delete inside the trash: one removal beneath the root, never
                    // following a link (fs::remove_all without one)
                    safewrite::removeTreeOrThrow(rootFor(file.originalFile), file.originalFile);
                } else {
                    // Normal deletion - move to trash with unique name. The trash is .trash (a hidden
                    // folder an MCP call's deny-by-default guard refuses): the editor names it so its
                    // own trash move goes through (util/SafeWrite.h, EditorInternal)
                    safewrite::EditorInternal trashDir(trash);
                    if (!fs::exists(trash)) {
                        safewrite::createDirOrThrow(rootFor(trash), trash);
                    }
                    file.trashFile = generateUniqueTrashPath(trash, file.originalFile);
                    safewrite::renameOrThrow(rootFor(file.originalFile), file.originalFile, file.trashFile);
                }

                deletedPaths.push_back(file.originalFile);
            }
        } catch (const fs::filesystem_error& e) {
            printf("Error: Deleting %s: %s\n", FileUtils::pathToGenericUtf8(file.originalFile).c_str(), e.what());
            return false;
        }
    }

    if (project) {
        for (const auto& deletedPath : deletedPaths) {
            project->cleanupMaterialFilePath(deletedPath);
            project->cleanupSceneFilePath(deletedPath);
            project->cleanupPrefabFilePath(deletedPath);
            project->cleanupScriptFilePath(deletedPath);
            project->cleanupAssetFilePath(deletedPath);
            project->cleanupShaderFilePath(deletedPath);
            // clip references stay (Missing); documents, the pool and the players learn
            project->cleanupClipFilePath(deletedPath);
            project->cleanupInspectedAsset(deletedPath);
        }
        if (deletedShaderSource)
            project->invalidateCustomShaders();
    }

    return true;
}

void editor::DeleteFileCmd::undo(){
    bool restoredShaderSource = false;
    for (DeleteFilesData& file : files){
        try {
            // Only attempt to restore if the file was moved to trash (not permanently deleted)
            if (!file.trashFile.empty() && fs::exists(file.trashFile)) {
                safewrite::renameOrThrow(rootFor(file.trashFile), file.trashFile, file.originalFile);
                restoredShaderSource |= fs::is_directory(file.originalFile) ||
                                        Util::isShaderFile(file.originalFile.string());
                // Prefabs and clips come back with their GUIDs: their Missing Prefab instances
                // and the clip references relink
                if (project) {
                    project->importPrefabFiles(file.originalFile);
                }
            }
        } catch (const fs::filesystem_error& e) {
            printf("Error: Restoring %s: %s\n", FileUtils::pathToGenericUtf8(file.originalFile).c_str(), e.what());
            undoProblem += (undoProblem.empty() ? "" : "; ") + std::string(e.what());
        }
    }
    if (project && restoredShaderSource)
        project->invalidateCustomShaders();
}
