// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CopyFileCmd.h"

#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "util/Util.h"
#include "vat/VatAssetFiles.h"

#include <algorithm>

using namespace jasperengine;

// A Vertex Animation moves and copies as a pair (vertex animation spec 2.1): the .vat or the
// .vat.glb brings the other file of its directory along (same name: the copied .vat still names
// its mesh, and gets a GUID of its own)
void editor::CopyFileCmd::addVatPairs(){
    const size_t count = files.size();
    for (size_t i = 0; i < count; i++) {
        const FileCopyData fdata = files[i];
        const fs::path pair = vat::existingPairOf(fdata.sourceDirectory / fdata.filename);
        if (pair.empty()) {
            continue;
        }
        const bool listed = std::any_of(files.begin(), files.end(), [&](const FileCopyData& other) {
            return (other.sourceDirectory / other.filename).lexically_normal() == pair.lexically_normal();
        });
        if (!listed) {
            FileCopyData added = fdata;
            added.filename = pair.filename();
            files.push_back(added);
        }
    }
}

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
    addVatPairs();
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
    addVatPairs();
}

void editor::CopyFileCmd::moveFile(const fs::path& source, const fs::path& destination, bool isDirectory){
    safewrite::renameOrThrow(rootFor(source), source, destination);
    if (!project) {
        return;
    }
    const std::string extension = source.extension().string();
    if (isDirectory || Util::isMaterialFile(extension)) {
        project->remapMaterialFilePath(source, destination);
    }
    if (isDirectory || Util::isSceneFile(extension)) {
        project->remapSceneFilePath(source, destination);
    }
    if (isDirectory || Util::isPrefabFile(extension)) {
        project->remapPrefabFilePath(source, destination);
    }
    if (isDirectory || Util::isScriptFile(extension)) {
        project->remapScriptFilePath(source, destination);
    }
    if (isDirectory || Util::isAssetFile(extension)) {
        project->remapAssetFilePath(source, destination);
    }
    // clips: their GUIDs follow, documents and players learn the new path
    // clips (their GUIDs follow) and models (their sub-clips): open documents follow, players reload
    if (isDirectory || Util::isGuidJsonAssetFile(extension) || Util::isModelFile(extension)) {
        project->remapClipFilePath(source, destination);
    }
    project->remapInspectedAsset(source, destination);
    if (isDirectory || Util::isShaderFile(extension)) {
        project->remapShaderFilePath(source, destination);
    }
}

std::vector<editor::Command::Touched> editor::CopyFileCmd::touchedPaths(bool undo) const{
    std::vector<Touched> out;
    for (const auto& fdata : files) {
        const fs::path source = fdata.sourceDirectory / fdata.filename;
        const fs::path destination = fdata.targetDirectory / fdata.filename;
        // a copy: its destination made (redo) or removed (undo); a move: both entries renamed
        if (!copy) out.push_back({source, true});
        out.push_back({destination, true});
    }
    return out;
}

bool editor::CopyFileCmd::execute(){
    bool touchedShaderSource = false;
    // moves: every file first, then the clips' hints (a clip moved in the same step is at its
    // final place when its hint is rewritten, whatever the order of the files)
    std::vector<std::pair<fs::path, fs::path>> moved;
    // what went (moved or copied): the Vertex Animation meshes among it name their textures again
    std::vector<std::pair<fs::path, fs::path>> went;

    for (const auto& fdata : files) {
        fs::path sourceFs = fdata.sourceDirectory / fdata.filename;
        fs::path destFs = fdata.targetDirectory / fdata.filename;
        try {
            if (fs::exists(sourceFs)) {
                bool isDir = fs::is_directory(sourceFs);
                std::string extension = sourceFs.extension().string();
                // Copies count too: a .glsl arriving in a directory a fork resolves
                // against changes which file wins for it.
                touchedShaderSource |= isDir || Util::isShaderFile(extension);

                if (copy){
                    const fs::path root = rootFor(destFs);
                    if (!root.empty()) {
                        // beneath the root: the source read wherever its links lead, the copy made
                        // with no link on the destination's way followed
                        std::string error;
                        if (!safewrite::copyResolvedBeneath(root, sourceFs, destFs, true, error)) {
                            throw fs::filesystem_error(error, sourceFs, destFs, std::make_error_code(std::errc::operation_not_permitted));
                        }
                    } else {
                        const auto options = fs::copy_options::overwrite_existing |
                            (isDir ? fs::copy_options::recursive : fs::copy_options::none);
                        fs::copy(sourceFs, destFs, options);
                    }
                    went.emplace_back(sourceFs, destFs);
                    // copied prefabs and clips are new assets: new GUIDs, the same ones on a redo
                    if (project && (isDir || AssetDatabase::isTrackedFile(destFs))) {
                        project->importPrefabFiles(destFs, true, &copyGuids);
                    }
                }else{
                    moveFile(sourceFs, destFs, isDir);
                    moved.emplace_back(sourceFs, destFs);
                    went.emplace_back(sourceFs, destFs);
                }
            }
        } catch (const fs::filesystem_error& e) {
            printf("Error: Moving/Copying %s: %s\n", sourceFs.string().c_str(), e.what());
            // what went before the failure stays usable (its meshes still name their textures)
            vatUriMoves = moved;
            vatUriEdits = vat::rebaseMovedVatMeshes(went);
            if (project && touchedShaderSource)
                project->invalidateCustomShaders();
            return false;
        }
    }

    if (project && touchedShaderSource)
        project->invalidateCustomShaders();

    // a .vat.glb in another directory names the same texture files (or the copies made with it)
    vatUriMoves = moved;
    vatUriEdits = vat::rebaseMovedVatMeshes(went);

    // clips previewing a moved model (or taken from it) name it again, in this step
    if (project) {
        for (const auto& [source, destination] : moved) {
            std::vector<ClipHintEdit> edits = remapClipHints(project, source, destination);
            clipHintEdits.insert(clipHintEdits.end(), edits.begin(), edits.end());
        }
    }

    return true;
}

void editor::CopyFileCmd::undo(){
    bool touchedShaderSource = false;
    restoreClipHints(project, clipHintEdits);
    clipHintEdits.clear();

    for (const auto& fdata : files) {
        fs::path sourceFs = fdata.targetDirectory / fdata.filename;
        fs::path destFs = fdata.sourceDirectory / fdata.filename;
        try {
            if (fs::exists(sourceFs)) {
                bool isDir = fs::is_directory(sourceFs);
                std::string extension = sourceFs.extension().string();
                // Copies count too: a .glsl arriving in a directory a fork resolves
                // against changes which file wins for it.
                touchedShaderSource |= isDir || Util::isShaderFile(extension);

                if (copy) {
                    // the copy goes: one removal beneath the root when there is one, never
                    // following a link (fs::remove_all without one)
                    safewrite::removeTreeOrThrow(rootFor(sourceFs), sourceFs);
                    // instances of the copies made since become Missing Prefabs
                    if (project && (isDir || Util::isPrefabFile(extension))) {
                        project->cleanupPrefabFilePath(sourceFs);
                    }
                    // references to the copied clips stay (Missing until a redo)
                    if (project && (isDir || Util::isGuidJsonAssetFile(extension))) {
                        project->cleanupClipFilePath(sourceFs);
                    }
                    if (project) {
                        project->cleanupInspectedAsset(sourceFs);
                    }
                }else{
                    moveFile(sourceFs, destFs, isDir);
                }
            }
        } catch (const fs::filesystem_error& e) {
            printf("Error: Undo moving/Copying %s: %s\n", FileUtils::pathToGenericUtf8(sourceFs).c_str(), e.what());
            undoProblem += (undoProblem.empty() ? "" : "; ") + std::string(e.what());
        }
    }

    // the moved meshes back where they were name their textures as before (copies are gone)
    if (!copy) {
        vat::restoreVatMeshUris(vatUriEdits, vatUriMoves);
    }
    vatUriEdits.clear();
    vatUriMoves.clear();

    if (project && touchedShaderSource)
        project->invalidateCustomShaders();
}
