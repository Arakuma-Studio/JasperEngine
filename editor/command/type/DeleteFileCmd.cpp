// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeleteFileCmd.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "util/Util.h"
#include "asset/AssetOps.h"
#include "Out.h"
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


// Deleting moves the files (metas included) to the project trash through AssetOps: the
// references to them stay and show as Missing, and an undo resolves them again (S04 R15)
bool editor::DeleteFileCmd::execute(){
    for (DeleteFilesData& file : files){
        std::error_code ec;
        if (!fs::exists(file.originalFile, ec)) {
            continue;
        }
        // what is already in the trash is deleted for good
        const fs::path relToTrash = file.originalFile.lexically_normal().lexically_relative(trash.lexically_normal());
        if (file.originalFile == trash || (!relToTrash.empty() && *relToTrash.begin() != "..")) {
            // one removal beneath the write scope's root, never following a link
            FileUtils::removeAll(file.originalFile, ec);
            file.trashFile.clear();
            continue;
        }
        // the trash is .trash (a hidden folder an MCP call's deny-by-default guard refuses): named
        // so the editor's own trash move goes through (util/SafeWrite.h, EditorInternal)
        safewrite::EditorInternal trashDir(trash);
        AssetOps::Result result;
        if (project) {
            result = AssetOps::remove(*project, file.originalFile, file.trashFile);
        } else {
            // no project (a folder that is none: the tests of the write scope): the file alone
            // into the trash, in the write scope beneath its root
            FileUtils::createDirectories(trash, ec);
            file.trashFile = AssetOps::uniqueTrashPath(trash, file.originalFile);
            result.ok = FileUtils::renamePath(file.originalFile, file.trashFile, ec);
            if (!result.ok) {
                result.error = safewrite::lastScopedError().empty() ? ec.message() : safewrite::lastScopedError();
                file.trashFile.clear();
            }
        }
        if (!result.ok) {
            Out::error("Couldn't delete \"%s\": %s", file.originalFile.filename().string().c_str(), result.error.c_str());
            // the ones deleted before come back: a failed command leaves nothing half done
            for (DeleteFilesData& done : files) {
                if (&done == &file) break;
                if (done.trashFile.empty()) continue;
                if (project) {
                    AssetOps::restore(*project, done.trashFile, done.originalFile);
                } else {
                    std::error_code ignored;
                    FileUtils::renamePath(done.trashFile, done.originalFile, ignored);
                }
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
    // the files come back out of .trash (the editor's own folder, util/SafeWrite.h)
    safewrite::EditorInternal trashDir(trash);
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        DeleteFilesData& file = *it;
        std::error_code ec;
        // only what went to the trash comes back (not what was deleted for good)
        if (file.trashFile.empty() || !fs::exists(file.trashFile, ec)) {
            continue;
        }
        AssetOps::Result result;
        if (project) {
            result = AssetOps::restore(*project, file.trashFile, file.originalFile);
        } else if (fs::exists(file.originalFile, ec)) {
            result.error = file.originalFile.generic_string() + " already exists";
        } else {
            result.ok = FileUtils::renamePath(file.trashFile, file.originalFile, ec);
            if (!result.ok) result.error = safewrite::lastScopedError().empty() ? ec.message() : safewrite::lastScopedError();
        }
        if (!result.ok) {
            Out::error("Couldn't restore \"%s\": %s", file.originalFile.filename().string().c_str(), result.error.c_str());
        }
    }
}
