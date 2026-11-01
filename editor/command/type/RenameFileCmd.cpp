// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RenameFileCmd.h"

#include "util/SafeWrite.h"

#include "util/FileUtils.h"
#include "util/Util.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatDocument.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;

editor::RenameFileCmd::RenameFileCmd(Project* project, std::string oldName, std::string newName, std::string directory){
    this->project = project;
    this->oldFilename = fs::path(oldName).filename();
    this->newFilename = fs::path(newName).filename();
    this->directory = fs::path(directory);
}

void editor::RenameFileCmd::moveThumbnail(const fs::path& oldThumbnail, const fs::path& renamedFile){
    if (!project || oldThumbnail.empty()) {
        return;
    }

    std::error_code ec;
    // Nothing to move when the file never had a cached thumbnail.
    if (!fs::exists(oldThumbnail, ec) || ec) {
        return;
    }

    fs::path newThumbnail = project->getThumbnailPath(renamedFile);
    if (newThumbnail.empty() || newThumbnail == oldThumbnail) {
        return;
    }

    // (in a write scope, beneath its root: FileUtils' scoped helpers). The thumbnail cache is the
    // editor's own .jasperengine/thumbs, which an MCP call's deny-by-default guard refuses: named
    // here so the editor's own move goes through (util/SafeWrite.h, EditorInternal)
    safewrite::EditorInternal ownThumbs(project->getThumbsDir());
    FileUtils::createDirectories(newThumbnail.parent_path(), ec);
    FileUtils::renamePath(oldThumbnail, newThumbnail, ec);
}

bool editor::RenameFileCmd::renameFile(const fs::path& sourceFs, const fs::path& destFs){
    if (!fs::exists(sourceFs)) {
        return false;
    }
    {
        bool isDir = fs::is_directory(sourceFs);
        // Resolve the thumbnail path before the rename, while the source still exists.
        fs::path oldThumbnail = project ? project->getThumbnailPath(sourceFs) : fs::path();
        safewrite::renameOrThrow(rootFor(sourceFs), sourceFs, destFs);
        if (project) {
            moveThumbnail(oldThumbnail, destFs);
            std::string extension = sourceFs.extension().string();
            if (isDir || Util::isMaterialFile(extension)) {
                project->remapMaterialFilePath(sourceFs, destFs);
            }
            if (isDir || Util::isSceneFile(extension)) {
                project->remapSceneFilePath(sourceFs, destFs);
            }
            // either name: a file renamed to or from .prefab starts or stops being one
            if (isDir || Util::isPrefabFile(extension) || Util::isPrefabFile(destFs.extension().string())) {
                project->remapPrefabFilePath(sourceFs, destFs);
            }
            if (isDir || Util::isScriptFile(extension)) {
                project->remapScriptFilePath(sourceFs, destFs);
            }
            if (isDir || Util::isAssetFile(extension)) {
                project->remapAssetFilePath(sourceFs, destFs);
            }
            // clips and Vertex Animations (their GUIDs follow) and models (their sub-clips): open
            // documents follow, players and animators reload
            if (isDir || Util::isGuidJsonAssetFile(extension) || Util::isGuidJsonAssetFile(destFs.extension().string()) ||
                Util::isModelFile(extension)) {
                project->remapClipFilePath(sourceFs, destFs);
            }
            project->remapInspectedAsset(sourceFs, destFs);
            if (isDir || Util::isShaderFile(extension)) {
                project->remapShaderFilePath(sourceFs, destFs);
                project->invalidateCustomShaders();
            }
        }
    }
    return true;
}

std::vector<editor::Command::Touched> editor::RenameFileCmd::touchedPaths(bool) const{
    std::vector<Touched> out = {{directory / oldFilename, true}, {directory / newFilename, true}};
    if (!pairOldName.empty()) {
        out.push_back({directory / pairOldName, true});
        out.push_back({directory / pairNewName, true});
    }
    // the renamed .vat's mesh field, written back
    if (!meshFieldFile.empty()) out.push_back({meshFieldFile, false});
    return out;
}

bool editor::RenameFileCmd::execute(){
    fs::path sourceFs = directory / oldFilename;
    fs::path destFs = directory / newFilename;

    // a Vertex Animation's other file goes too; its new name must be free
    if (!pairDecided) {
        pairDecided = true;
        fs::path pairOld, pairNew;
        if (vat::renamedPair(sourceFs, destFs, pairOld, pairNew)) {
            pairOldName = pairOld.filename();
            pairNewName = pairNew.filename();
        }
    }
    const bool withPair = !pairOldName.empty();
    std::error_code ec;
    // a pair member keeps its extension (the other file couldn't follow)
    const std::string refusal = vat::pairRenameRefusal(sourceFs, destFs, oldFilename.string());
    if (!refusal.empty()) {
        printf("Error: Renaming %s: %s\n", sourceFs.string().c_str(), refusal.c_str());
        return false;
    }
    if (withPair && fs::exists(directory / pairNewName, ec) && fs::exists(directory / pairOldName, ec)) {
        printf("Error: Renaming %s: %s already exists (a Vertex Animation's .vat and .vat.glb are renamed together)\n",
               sourceFs.string().c_str(), (directory / pairNewName).string().c_str());
        return false;
    }

    bool renamed = false;
    try {
        renamed = renameFile(sourceFs, destFs);
    } catch (const fs::filesystem_error& e) {
        printf("Error: Renaming %s: %s\n", FileUtils::pathToGenericUtf8(sourceFs).c_str(), e.what());
        return false;
    }
    // the pair is renamed whole or not at all: the other file failing puts the first one back
    if (renamed && withPair) {
        try {
            renameFile(directory / pairOldName, directory / pairNewName);
        } catch (const fs::filesystem_error& e) {
            printf("Error: Renaming %s: %s (%s keeps its name too)\n", (directory / pairOldName).string().c_str(), e.what(),
                   sourceFs.string().c_str());
            try {
                renameFile(destFs, sourceFs);
            } catch (const fs::filesystem_error& back) {
                printf("Error: Renaming %s back: %s\n", destFs.string().c_str(), back.what());
            }
            return false;
        }
    }

    // the renamed .vat names its renamed mesh (the payload is the same: no rebake)
    meshFieldFile.clear();
    if (renamed && withPair) {
        const fs::path vatFile = Util::isVertexAnimationFile(destFs.string()) ? destFs : directory / pairNewName;
        std::ifstream file(vatFile, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        std::string updated = text;
        if (vat::setMeshField(updated, vat::pairPathOf(vatFile).filename().generic_string()) &&
            FileUtils::writeFileReplacing(vatFile, updated)) {
            meshFieldFile = vatFile;
            meshFieldBefore = text;
            meshFieldAfter = updated;
            if (project) {
                // the database learns the new size and time (same GUID), the documents reload
                project->importPrefabFiles(vatFile);
                VatDocument::filesChanged({vatFile}, project);
            }
        }
    }

    // clips previewing the renamed model (or taken from it) name it again, in this step
    clipHintEdits.clear();
    if (renamed) {
        clipHintEdits = remapClipHints(project, sourceFs, destFs);
    }

    return true;
}

void editor::RenameFileCmd::undo(){
    fs::path sourceFs = directory / newFilename;
    fs::path destFs = directory / oldFilename;
    restoreClipHints(project, clipHintEdits);
    clipHintEdits.clear();
    if (!meshFieldFile.empty()) {
        std::ifstream file(meshFieldFile, std::ios::binary);
        const std::string now((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        // edited since (an Inspector edit undone separately keeps its own text)
        if (now == meshFieldAfter) {
            FileUtils::writeFileReplacing(meshFieldFile, meshFieldBefore);
        }
        meshFieldFile.clear();
    }
    try {
        renameFile(sourceFs, destFs);
        if (!pairOldName.empty()) {
            renameFile(directory / pairNewName, directory / pairOldName);
        }
    } catch (const fs::filesystem_error& e) {
        printf("Error: Undo renaming %s: %s\n", FileUtils::pathToGenericUtf8(sourceFs).c_str(), e.what());
        undoProblem += (undoProblem.empty() ? "" : "; ") + std::string(e.what());
    }
    if (project && !pairOldName.empty()) {
        const fs::path vatFile = Util::isVertexAnimationFile(destFs.string()) ? destFs : directory / pairOldName;
        project->importPrefabFiles(vatFile);
        VatDocument::filesChanged({vatFile}, project);
    }
}
